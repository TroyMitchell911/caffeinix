#ifndef __CAFFEINIX_KERNEL_SCHEDULER_H
#define __CAFFEINIX_KERNEL_SCHEDULER_H

#include <thread.h>
#include <process.h>

struct device_node;

typedef struct cpu {
        struct context context;
        // thread_t thread;
        process_t proc;
        /* Nesting Depth */
        uint8 lock_nest_depth;
        /* Is the interrupt enabled before locking */
        uint8 before_lock;
	uint64 hart_id;
	struct device_node *of_node;
	volatile uint8 online;
}*cpu_t;

uint8 cpuid(void);
cpu_t cur_cpu(void);
// thread_t cur_thread();
process_t cur_proc();
void scheduler(void);
void yield(void);
void sched(void);

#endif
