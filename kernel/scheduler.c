#include <scheduler.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <riscv.h>
#include <list.h>
#include <debug.h>

extern void switchto(context_t c, context_t p);
struct cpu cpus[NCPU];

static struct {
        struct spinlock lock;
        struct list runnable;
        uint32 count;
} runqueue;

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

void scheduler_init(void)
{
        spinlock_init(&runqueue.lock, "runqueue");
        list_init(&runqueue.runnable);
        runqueue.count = 0;
}

/* The caller must hold thread->lock. */
void scheduler_make_runnable(thread_t thread)
{
        if(!spinlock_holding(&thread->lock))
                PANIC("runnable lock");

        spinlock_acquire(&runqueue.lock);
        if(thread->on_runqueue ||
           (thread->state != THREAD_ALLOCATED &&
            thread->state != THREAD_RUNNING &&
            thread->state != THREAD_SLEEPING))
                PANIC("invalid runnable thread");
        thread->state = THREAD_RUNNABLE;
        list_insert_before(&runqueue.runnable, &thread->run_node);
        thread->on_runqueue = 1;
        runqueue.count++;
        spinlock_release(&runqueue.lock);
}

static thread_t scheduler_dequeue(void)
{
        thread_t thread = 0;
        list_t node;

        spinlock_acquire(&runqueue.lock);
        if(runqueue.count) {
                node = runqueue.runnable.next;
                if(node == &runqueue.runnable)
                        PANIC("empty runqueue");
                thread = list_entry(node, struct thread, run_node);
                if(!thread->on_runqueue)
                        PANIC("unqueued thread");
                list_remove(node);
                thread->on_runqueue = 0;
                runqueue.count--;
        } else if(runqueue.runnable.next != &runqueue.runnable) {
                PANIC("runqueue count");
        }
        spinlock_release(&runqueue.lock);
        return thread;
}

void scheduler(void)
{
        volatile cpu_t cpu = cur_cpu();
        thread_t t;

        cpu->current = 0;

        for(;;) {
                /* Open interrupt to avoid dead lock */
                intr_on();

                t = scheduler_dequeue();
                if(!t)
                        continue;

                spinlock_acquire(&t->lock);
                if(t->state != THREAD_RUNNABLE || t->on_runqueue ||
                   !t->home)
                        PANIC("invalid scheduled thread");
                t->state = THREAD_RUNNING;
                t->home->tinfo->addr = TRAPFRAME(t->id_p);
                cpu->current = t;
                switchto(&cpu->context, &t->context);
                cpu->current = 0;
                spinlock_release(&t->lock);
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
        scheduler_make_runnable(current);
        sched();
        spinlock_release(&current->lock);
}

void scheduler_wake(thread_t thread)
{
        spinlock_acquire(&thread->lock);
        if(thread->state == THREAD_SLEEPING)
                scheduler_make_runnable(thread);
        spinlock_release(&thread->lock);
}
