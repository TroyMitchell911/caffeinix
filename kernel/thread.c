#include <thread.h>
#include <palloc.h>
#include <string.h>
#include <uart.h>
#include <printf.h>

struct list all_thread;

extern volatile uint64 tick_count;


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
                        printf("test1\n");
                }
        }
}

static void test2(void* arg)
{
        uint64 last_tick = tick_count;

        while(1) {
                if(tick_count - last_tick >= 10) {
                        last_tick = tick_count;
                        printf("test2\n");
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

