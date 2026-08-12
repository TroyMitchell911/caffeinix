#include <scheduler.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <riscv.h>
#include <list.h>
#include <debug.h>

extern void switchto(context_t c, context_t p);
struct cpu cpus[NCPU];

/* Get hart id */
uint8 cpuid(void)
{
        uint8 hartid = tp_r();
        return hartid;  
}

/* Get current cpu structure */
cpu_t cur_cpu(void)
{
        uint8 hartid = cpuid();
        cpu_t cpu = &cpus[hartid];
        return cpu;
}

/* Get current thread */
thread_t cur_thread(void)
{
        return cur_cpu()->current;
}

/* Get current process */
process_t cur_proc(void)
{
        thread_t current = cur_thread();

        return current ? current->home : 0;
}

void scheduler(void)
{
        volatile cpu_t cpu = cur_cpu();
        thread_t t;

        cpu->current = 0;

        for(;;) {
                /* Open interrupt to avoid dead lock */
                intr_on();

                for(t = &thread[0]; t <= &thread[NTHREAD - 1]; t ++) {
			int ret = spinlock_trylock(&t->lock);
			if (ret)
				continue;

                        if(t->state == THREAD_RUNNABLE) {
                                if(!t->home)
                                        PANIC("scheduler");
                                t->state = THREAD_RUNNING;
                                t->home->tinfo->addr = TRAPFRAME(t->id_p);
                                cpu->current = t;
                                switchto(&cpu->context, &t->context);
                                cpu->current = 0;
                        }
                        spinlock_release(&t->lock);
                }
        } 
}

/* Change the context to kernel scheduler */
void sched(void)
{
        cpu_t cpu = cur_cpu();
        thread_t current = cpu->current;
        uint8 before_lock;

        if(!current || !spinlock_holding(&current->lock)) {
                PANIC("sched holding");
        }

        if(intr_status()) {
                PANIC("sched intr open");
        }

        if(cpu->lock_nest_depth != 1) {
                printf("%d->", cpu->lock_nest_depth);
                PANIC("sched lock_nest_depth");
        }

        if(current->state == THREAD_RUNNING) {
                PANIC("sched running");
        }


        /* Save the value of lock */
        before_lock = cpu->before_lock;
        /* Change the context to  kernel scheduler */
        switchto(&current->context, &cpu->context);
	/* A resumed thread may have migrated to another CPU. */
	cur_cpu()->before_lock = before_lock;
}

void yield(void)
{
        thread_t current = cur_thread();

        spinlock_acquire(&current->lock);
        current->state = THREAD_RUNNABLE;
        sched();
        spinlock_release(&current->lock);
}

void scheduler_wake(thread_t thread)
{
        spinlock_acquire(&thread->lock);
        if(thread->state == THREAD_SLEEPING)
                thread->state = THREAD_RUNNABLE;
        spinlock_release(&thread->lock);
}
