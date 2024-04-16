#include <thread.h>
#include <palloc.h>
#include <string.h>
#include <kernel_config.h>
#include <uart.h>

static struct list all_thread;
struct cpu cpus[NCPU];

extern void switchto(context_t c, context_t p);
extern volatile uint64 tick_count;

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


void thread_create(const char* name, thread_func_t func, void* arg)
{
        thread_t thread = (thread_t)palloc();

        /* Init the spinlock */
        spinlock_init(&thread->lock, name);
        spinlock_acquire(&thread->lock);

        thread->state = READY;
        thread->name = name;

        list_init(&thread->all_tag);
        /* Insert node in the head */
        list_insert_after(&all_thread, &thread->all_tag);
        /* Clear the context */
        memset(&thread->context, 0, sizeof(struct context));
        /* Set the sp of thread to the end of memory we alloced */
        thread->context.sp = (uint64)thread + PGSIZE;
        /* Set the return address of thread to the address of func */
        thread->context.ra = (uint64)func;

        spinlock_release(&thread->lock);
}

static void test1(void* arg)
{
        uint64 last_tick = tick_count;

        while(1) {
                if(tick_count - last_tick >= 10) {
                        last_tick = tick_count;
                        uart_puts("test1\n");
                }
        }
}

static void test2(void* arg)
{
        uint64 last_tick = tick_count;

        while(1) {
                if(tick_count - last_tick >= 10) {
                        last_tick = tick_count;
                        uart_puts("test2\n");
                }
        }
}

void thread_init(void)
{
        list_init(&all_thread);
}

void thread_test(void) 
{
        thread_create("test1", test1, 0);
        thread_create("test2", test2, 0);
}

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