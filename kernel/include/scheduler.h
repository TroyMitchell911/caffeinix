/*
 * Scheduler interface.
 *
 * The scheduler owns runnable-state transitions and selects the next thread
 * from the global vruntime-ordered runqueue.  Thread locks serialize an
 * individual thread's state; the private runqueue lock serializes queue and
 * CPU selection state.  Callers must not change a thread state directly.
 */
#ifndef __CAFFEINIX_KERNEL_SCHEDULER_H
#define __CAFFEINIX_KERNEL_SCHEDULER_H

#include <thread.h>
#include <process.h>

struct device_node;

typedef struct cpu {
        struct context context;
        thread_t current;
	/* Removed from the runqueue, but not yet switched in. */
	thread_t selected;
	/* Interrupt-disable nesting maintained by spinlock code. */
        uint8 lock_nest_depth;
	/* Interrupt state saved at the outermost spinlock acquisition. */
        uint8 before_lock;
	uint64 hart_id;
	struct device_node *of_node;
	void *scheduler_stack;
	void *secondary_boot_stack;
	volatile uint8 online;
	uint8 idle;
	volatile uint8 need_resched;
	volatile uint64 membarrier_request;
	volatile uint64 membarrier_done;
	volatile uint64 idle_since_ns;
	volatile uint64 idle_time_ns;
}*cpu_t;

extern cpu_t *cpus;

/**
 * cpuid() - Return the logical CPU number of the calling hart
 *
 * Context: Any context after per-CPU state is established.
 *
 * Return: A zero-based logical CPU number.
 */
int cpuid(void);
/**
 * cur_cpu() - Return the calling CPU's scheduler state
 *
 * Context: Any context after per-CPU state is established.
 *
 * Return: The calling CPU's persistent state.
 */
cpu_t cur_cpu(void);
/**
 * cur_thread() - Snapshot the thread running on this CPU
 *
 * Interrupts are disabled while reading the per-CPU current pointer because
 * a timer interrupt can otherwise schedule a different thread.
 *
 * Context: Any context. May return NULL before scheduling begins.
 *
 * Return: The current thread, or NULL.
 */
thread_t cur_thread(void);
/**
 * cur_proc() - Return the current thread's owning process
 *
 * Context: Any context. Kernel threads and early boot have no process.
 *
 * Return: The current process, or NULL.
 */
process_t cur_proc(void);
/**
 * scheduler_init() - Initialize the runqueue before threads are runnable
 *
 * Context: Early boot with no concurrent scheduler users.
 */
void scheduler_init(void);
/**
 * scheduler() - Run the per-CPU scheduler loop
 *
 * This function never returns. It switches into selected threads and reaps
 * exited threads after they switch back.
 *
 * Context: Per-CPU scheduler stack with interrupts initially disabled.
 */
void scheduler(void);
/**
 * yield() - Requeue the current running thread and select another thread
 *
 * Context: Thread context; may switch and returns with no thread lock held.
 */
void yield(void);
/**
 * sched() - Switch the current non-running thread back to its scheduler
 *
 * The caller holds the current thread lock and has changed its state from
 * THREAD_RUNNING. The thread lock remains held when this thread resumes.
 *
 * Context: Thread context with interrupts disabled and exactly one lock held.
 */
void sched(void);
/**
 * scheduler_exit() - Mark and switch away from the current thread
 *
 * Context: Thread context. Never returns.
 */
void scheduler_exit(void);
/**
 * scheduler_exit_locked() - Exit with the current thread lock already held
 *
 * Context: Thread context with the current thread lock held. Never returns.
 */
void scheduler_exit_locked(void);
/**
 * scheduler_make_runnable() - Queue an allocated, sleeping, or running thread
 * @thread: Caller-locked thread to enqueue.
 *
 * Idle CPUs are preferred; otherwise a sufficiently unfair running CPU is
 * asked to reschedule.
 *
 * Context: Thread context with @thread->lock held.
 */
void scheduler_make_runnable(thread_t thread);
/**
 * scheduler_kick() - Request that a thread's CPU reaches a reschedule point
 * @thread: Thread that may currently be running.
 *
 * Context: Any thread context. A remote CPU is interrupted with an IPI.
 */
void scheduler_kick(thread_t thread);
/**
 * scheduler_block_current() - Mark the current running thread sleeping
 *
 * Context: Current thread lock held. The caller subsequently invokes sched().
 */
void scheduler_block_current(void);
/**
 * scheduler_inherit() - Copy scheduling attributes to a new child
 * @child: Allocated, caller-locked child not on the runqueue.
 * @parent: Parent whose current vruntime and nice setting are inherited.
 *
 * Context: Thread context with @child->lock held.
 */
void scheduler_inherit(thread_t child, thread_t parent);
/**
 * scheduler_set_nice() - Set a thread's nice value
 * @thread: Thread to update.
 * @nice: Nice value in the inclusive range -20 to 19.
 *
 * Return: 0 on success, or -1 for an invalid thread or nice value.
 */
int scheduler_set_nice(thread_t thread, int nice);
/**
 * scheduler_get_nice() - Read a thread's nice value
 * @thread: Thread to inspect.
 *
 * Return: The nice value, or 0 when @thread is NULL.
 */
int scheduler_get_nice(thread_t thread);
/**
 * scheduler_request_resched() - Mark the local running CPU for rescheduling
 *
 * Context: Local CPU context; safe from interrupt paths.
 */
void scheduler_request_resched(void);
/**
 * scheduler_tick() - Evaluate local slice expiry and fairness preemption
 *
 * Context: Local timer interrupt or equivalent non-sleeping context.
 */
void scheduler_tick(void);
/**
 * scheduler_should_resched() - Consume the local reschedule request
 *
 * Context: Local CPU context at a trap-return safe point.
 *
 * Return: Nonzero once for a pending request, otherwise zero.
 */
int scheduler_should_resched(void);
/**
 * scheduler_account_kernel_enter() - Charge elapsed user mode time
 *
 * Context: Current thread enters the kernel; does not sleep.
 */
void scheduler_account_kernel_enter(void);
/**
 * scheduler_account_user_enter() - Charge elapsed kernel mode time
 *
 * Context: Current thread returns to user mode; does not sleep.
 */
void scheduler_account_user_enter(void);
/**
 * scheduler_thread_times() - Snapshot a thread's accumulated CPU times
 * @thread: Thread to inspect.
 * @now: Current monotonic time in nanoseconds.
 * @user: Destination for user-mode nanoseconds.
 * @system: Destination for kernel-mode nanoseconds.
 *
 * Running time since the last accounting boundary is included in the result.
 *
 * Context: @thread must remain live; @user and @system must be non-NULL.
 */
void scheduler_thread_times(thread_t thread, uint64 now,
			    uint64 *user, uint64 *system);
/**
 * scheduler_idle_time_ns() - Sum completed and current CPU idle time
 *
 * Return: Monotonic aggregate idle time in nanoseconds.
 */
uint64 scheduler_idle_time_ns(void);
/**
 * scheduler_context_switches() - Read the global switch counter
 *
 * Return: Number of switches since scheduler initialization.
 */
uint64 scheduler_context_switches(void);

#endif
