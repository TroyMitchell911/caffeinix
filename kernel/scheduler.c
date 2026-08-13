#include <scheduler.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <riscv.h>
#include <list.h>
#include <debug.h>
#include <cpu.h>
#include <sbi.h>

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

static int scheduler_claim_idle_cpu(void)
{
        cpu_t current = cur_cpu();
        int logical;

        if(current->idle) {
                current->idle = 0;
                return -1;
        }
        for(logical = 0; logical < cpu_count(); logical++) {
                if(&cpus[logical] == current ||
                   !cpus[logical].online || !cpus[logical].idle)
                        continue;
                cpus[logical].idle = 0;
                return logical;
        }
        return -1;
}

static void scheduler_enqueue(thread_t thread, int wake_idle)
{
        int target = -1;

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
        if(wake_idle)
                target = scheduler_claim_idle_cpu();
        spinlock_release(&runqueue.lock);

        if(target >= 0 && sbi_send_ipi(cpu_hart_id(target)))
                PANIC("SBI send IPI failed");
}

/* The caller must hold thread->lock. */
void scheduler_make_runnable(thread_t thread)
{
        scheduler_enqueue(thread, 1);
}

static thread_t scheduler_next(cpu_t cpu)
{
        thread_t thread = 0;
        list_t node;

        if(intr_status())
                PANIC("scheduler interrupts enabled");
        spinlock_acquire(&runqueue.lock);
        cpu->idle = 0;
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
        } else {
                cpu->idle = 1;
        }
        spinlock_release(&runqueue.lock);
        return thread;
}

void scheduler(void)
{
        volatile cpu_t cpu = cur_cpu();
        thread_t t;

        cpu->current = 0;
        cpu->idle = 0;

        for(;;) {
                /*
                 * Keep global interrupts disabled through WFI. An IPI that
                 * arrives after the empty check then remains pending and
                 * makes WFI return instead of being consumed too early.
                 */
                intr_off();
                t = scheduler_next(cpu);
                if(!t) {
                        wait_for_interrupt();
                        intr_on();
                        continue;
                }
                intr_on();

                spinlock_acquire(&t->lock);
                if(t->state != THREAD_RUNNABLE || t->on_runqueue ||
                   !t->home)
                        PANIC("invalid scheduled thread");
                t->state = THREAD_RUNNING;
                t->home->tinfo->addr = TRAPFRAME(t->id_p);
                cpu->current = t;
		cpu->need_resched = 0;
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
        scheduler_enqueue(current, 0);
        sched();
        spinlock_release(&current->lock);
}
void scheduler_exit_locked(void)
{
	thread_t current = cur_thread();

	if (!current || !spinlock_holding(&current->lock) ||
	    current->state != THREAD_RUNNING)
		PANIC("exit non-running thread");
	current->state = THREAD_EXITED;
	sched();
	PANIC("scheduled exited thread");
}

void scheduler_exit(void)
{
	thread_t current = cur_thread();

	if (!current)
		PANIC("exit without thread");
	spinlock_acquire(&current->lock);
	scheduler_exit_locked();
}

void scheduler_request_resched(void)
{
	cpu_t cpu = cur_cpu();

	if (cpu->current)
		cpu->need_resched = 1;
}

int scheduler_should_resched(void)
{
        cpu_t cpu = cur_cpu();

        if(!cpu->need_resched || !cpu->current ||
           cpu->current->state != THREAD_RUNNING)
                return 0;
        cpu->need_resched = 0;
        return 1;
}
