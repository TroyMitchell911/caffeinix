#ifndef __CAFFEINIX_KERNEL_SCHEDULER_H
#define __CAFFEINIX_KERNEL_SCHEDULER_H

#include <thread.h>
#include <process.h>

struct device_node;

typedef struct cpu {
        struct context context;
        thread_t current;
	/* Picked from the runqueue but not running yet. */
	thread_t selected;
        /* Nesting Depth */
        uint8 lock_nest_depth;
        /* Is the interrupt enabled before locking */
        uint8 before_lock;
	uint64 hart_id;
	struct device_node *of_node;
	void *scheduler_stack;
	volatile uint8 online;
	uint8 idle;
	volatile uint8 need_resched;
}*cpu_t;

uint8 cpuid(void);
cpu_t cur_cpu(void);
thread_t cur_thread(void);
process_t cur_proc(void);
void scheduler_init(void);
void scheduler(void);
void yield(void);
void sched(void);
void scheduler_exit(void);
void scheduler_exit_locked(void);
void scheduler_make_runnable(thread_t thread);
void scheduler_block_current(void);
void scheduler_inherit(thread_t child, thread_t parent);
int scheduler_set_nice(thread_t thread, int nice);
int scheduler_get_nice(thread_t thread);
void scheduler_request_resched(void);
void scheduler_tick(void);
int scheduler_should_resched(void);

#endif
