#ifndef __CAFFEINIX_KERNEL_THREAD_H
#define __CAFFEINIX_KERNEL_THREAD_H

#include <typedefs.h>
#include <list.h>

typedef struct context {
        uint64 ra;
        uint64 sp;

        /* Callee saved */
        uint64 s0;
        uint64 s1;
        uint64 s2;
        uint64 s3;
        uint64 s4;
        uint64 s5;
        uint64 s6;
        uint64 s7;
        uint64 s8;
        uint64 s9;
        uint64 s10;
        uint64 s11;
}*context_t;

typedef struct cpu {
        /* Nesting Depth */
        uint8 lock_nest_depth;
        /* Is the interrupt enabled before locking */
        uint8 before_lock;
}*cpu_t;

typedef struct thread {
        char name[16];
        struct context context;
        uint16 init_prio;
        uint16 current_prio;
        list_t tag;
}*thread_t;

typedef void (*thread_func_t)(void*);

void thread_init(void);
uint8 cpuid(void);
cpu_t cur_cpu(void);

#endif