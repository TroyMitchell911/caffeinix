#include <scheduler.h>
#include <kernel_config.h>
#include <riscv.h>
#include <list.h>

extern struct list all_thread;
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
thread_t cur_thread()
{
        return cur_cpu()->thread;
}

/* 
        Them must be called with interrupts disabled above functions
        to prevent race with thread being moved to a different CPU
*/

void scheduler(void)
{
        volatile cpu_t cpu = cur_cpu();
        thread_t thread;
        list_t p;
        
        cpu->thread = 0;
        for(;;) {
                /* Open interrupt to avoid dead lock */
                intr_on();
               
                for(p = all_thread.next; p != &all_thread; p = p->next) {
                        thread = list_entry(p, struct thread, all_tag);
                        if(!thread)
                                continue;
                        
                        if(thread->state == READY) {
                                // spinlock_acquire(&thread->lock);
                                thread->state = ACTIVE;
                                cpu->thread = thread;
                                list_remove(&thread->all_tag);
                                // spinlock_release(&thread->lock);
                                switchto(&cpu->context, &thread->context);
                                // break;
                        }
                        // spinlock_release(&thread->lock);
                }
        }
        
}

/* Change the context to kernel scheduler */
void sched(void)
{
        cpu_t cpu = cur_cpu();
        /* Save the value of lock */
        uint8 before_lock = cpu->before_lock;
        /* Change the context to  kernel scheduler */
        switchto(&cpu->thread->context, &cpu->context);
        /* Restore the value of lock */
        cpu->before_lock = before_lock;
}

/* Give up the cpu */
void yield(void)
{
        cpu_t cpu = cur_cpu();
        // spinlock_acquire(&cpu->thread->lock);
        /* Modify the value of state to READY */
        cpu->thread->state = READY;
        /* Insert node in the tail */
        list_insert_before(&all_thread, &cpu->thread->all_tag);
        // spinlock_release(&cpu->thread->lock);
        sched();
        
}