#include <thread.h>
#include <palloc.h>
#include <string.h>
#include <kernel_config.h>

static list_t all_thread;
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

void thread_init(void)
{
        list_init(all_thread);
}

void thread_create(const char* name, uint16 prio, thread_func_t func, void* arg)
{
        thread_t thread = (thread_t)palloc();
        memset(&thread->context, 0, sizeof(struct context));
        thread->context.sp = (uint64)thread + PGSIZE;
}