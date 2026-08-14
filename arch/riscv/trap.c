#include <trap.h>
#include <spinlock.h>
#include <debug.h>
#include <irq.h>
#include <scheduler.h>
#include <printf.h>
#include <plic.h>
#include <printk.h>
#include <timer.h>
#include <wait.h>

extern void kernel_vec(void);
extern char trampoline[], user_vec[], user_ret[];
extern void syscall(void);

void user_trap_ret(void);

struct spinlock tick_lock;
/* For test */
volatile uint64 tick_count = 0;

static void tick_intr(void)
{
        spinlock_acquire(&tick_lock);

        tick_count ++;
        spinlock_release(&tick_lock);
}

static int dev_intr(uint64 scause)
{
        int irq = 0;

        if((scause & 0x8000000000000000L) &&
           (scause & 0xff) == 1) {
                sip_clear_ssip();
                return 3;
        }
        /* This is a supervisor external interrupt via PLIC */
        else if((scause & 0x8000000000000000L) &&
                (scause & 0xff) == 9) {
                /* Get interrupt number from PLIC */
		irq = plic_claim();
		if (irq && irq_dispatch(irq) != IRQ_HANDLED)
			pr_err("irq: unhandled interrupt %d", irq);
                if(irq) {
                        /* Clear the interrupt flag */
                        plic_complete(irq);
                }
                return 1;
        } else if(scause == 0x8000000000000005L) {
		/* Supervisor timer interrupt delivered through SBI TIME. */
		timer_interrupt();
		wait_queue_expire(time_r());
                if(cpuid() == 0) {
                        tick_intr();
		}
                return 2;
        }   
        return 0;
}

void kernel_trap(void)
{
        uint8 which_dev = 0;
        uint64 sepc = sepc_r();
        uint64 sstatus = sstatus_r();
        uint64 cause = scause_r();

        if(!(sstatus & SSTATUS_SPP)) 
                PANIC("[kernel_trap]It is not from supervisor mode");
        if(intr_status())
                PANIC("[kernel_trap]Interrupt enabled");

        /* Unknown device interrupt */
        if((which_dev = dev_intr(cause)) == 0) {
                printf("scause %p\n", cause);
                printf("sepc=%p stval=%p\n", sepc, stval_r());
                PANIC("kerneltrap");
        }

	if (which_dev == 2)
		scheduler_tick();
	else if (which_dev == 3)
                scheduler_request_resched();

        if(scheduler_should_resched())
                yield();

        sepc_w(sepc);
        sstatus_w(sstatus);
}

extern void exit(int cause);

__attribute__((noreturn)) void kernel_stack_overflow(uint64 stack_pointer)
{
	printf_enter_panic();
	printf("kernel stack overflow: CPU=%d sp=%p\n", cpuid(),
	       stack_pointer);
	printf("scause=%p sepc=%p stval=%p\n", scause_r(), sepc_r(),
	       stval_r());
	PANIC("kernel stack overflow");
	for (;;)
		;
}

void user_trap_entry(void)
{
        int which_dev = 0;
        process_t p = cur_proc();
        uint64 cause = scause_r();

        if((sstatus_r() & SSTATUS_SPP)) {
                PANIC("Not from user mode");
        }

        stvec_w((uint64)kernel_vec);

        cur_thread()->trapframe->epc = sepc_r();

        if(cause == 8) {
                if(killed(p))
                        exit(-1);

                /* System call */
                cur_thread()->trapframe->epc += 4;
                intr_on();
                syscall();
        } else {
                if((which_dev = dev_intr(cause)) == 0) {
                        printf("scause %p\n", cause);
                        printf("sepc=%p stval=%p\n",
                               cur_thread()->trapframe->epc, stval_r());
                        PANIC("user_trap_entry");
                }
        }

        if(killed(p))
                exit(-1);

	if (which_dev == 2)
		scheduler_tick();
	else if (which_dev == 3)
                scheduler_request_resched();

        if(scheduler_should_resched())
                yield();

        user_trap_ret();
}

void user_trap_ret(void)
{
        process_t p;
        uint64 sstatus;
        uint64 satp;
        uint64 trampoline_uservec;
        uint64 trampoline_userret;

        p = cur_proc();
        /* Turn off the interrupt until we' are back in user space */
        intr_off();

        trampoline_uservec = TRAMPOLINE + (user_vec - trampoline);
        stvec_w(trampoline_uservec);

        cur_thread()->trapframe->kernel_satp = satp_r();
	cur_thread()->trapframe->kernel_sp =
		cur_thread()->kstack + KSTACK_SIZE;
        cur_thread()->trapframe->kernel_hartid = tp_r();
        cur_thread()->trapframe->kernel_trap = (uint64)user_trap_entry;

        sstatus = sstatus_r();
        /* Set the interrupt is from user mode */
        sstatus &= ~SSTATUS_SPP; 
	sstatus &= ~SSTATUS_FS_MASK;
	sstatus |= SSTATUS_FS_DIRTY;
        /* Enable interrupt */
        sstatus |= SSTATUS_SPIE; 
        sstatus_w(sstatus);

        /* Write the epc. It will be set to 0 if the process is first started */
        sepc_w(cur_thread()->trapframe->epc);

        satp = MAKE_SATP(p->pagetable);

        trampoline_userret = TRAMPOLINE + (user_ret - trampoline);
        /* Call user_ret */
        ((void (*)(uint64))trampoline_userret)(satp);
}

/* This function for first hart */
void trap_init_lock(void)
{
        spinlock_init(&tick_lock, "trap_tick");
}

/* This function for any hart */
void trap_init(void)
{
        stvec_w((uint64)kernel_vec);
	sie_w(sie_r() | SIE_SEIE | SIE_SSIE);
}
