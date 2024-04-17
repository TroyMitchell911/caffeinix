#ifndef __CAFFEINIX_KERNEL_SCHEDULER_H
#define __CAFFEINIX_KERNEL_SCHEDULER_H

#include <thread.h>

typedef struct cpu {
        struct context context;
        thread_t thread;
        /* Nesting Depth */
        uint8 lock_nest_depth;
        /* Is the interrupt enabled before locking */
        uint8 before_lock;
}*cpu_t;

uint8 cpuid(void);
cpu_t cur_cpu(void);
thread_t cur_thread();
void scheduler(void);
void yield(void);

#endif