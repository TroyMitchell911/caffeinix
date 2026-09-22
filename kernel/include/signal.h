/*
 * Linux RISC-V signal state and delivery interface.
 *
 * Process-directed signals live in process state and are retargeted to one
 * eligible thread. Thread-directed signals live in that thread.  The owning
 * process lock protects pending state, masks, actions, and target selection.
 */
#ifndef __CAFFEINIX_KERNEL_SIGNAL_H
#define __CAFFEINIX_KERNEL_SIGNAL_H

#include <list.h>
#include <typedefs.h>

#define SIGNAL_COUNT 64
#define SIGNAL_RESTART_SYS 512
#define SIGNAL_QUEUE_FULL -2
#define SIGNAL_QUEUE_DENIED -3

struct process;
struct thread;

struct signal_info {
	int signal;
	int error;
	int code;
	int sender_pid;
	uint32 sender_uid;
	uint32 sender_euid;
	int sender_sid;
	int status;
	uint64 address;
};

struct signal_pending {
	uint64 bits;
	struct signal_info information[SIGNAL_COUNT];
	struct list realtime;
	uint32 realtime_count;
};

/**
 * signal_thread_init() - Initialize a thread's signal state
 * @thread: Thread whose storage is not yet visible to signal senders.
 *
 * Context: Allocation or reset path; does not sleep.
 */
void signal_thread_init(struct thread *thread);
/**
 * signal_thread_destroy() - Drop pending signals owned by a thread
 * @thread: Thread being reset or freed.
 *
 * Context: Reaping path; frees queued realtime entries.
 */
void signal_thread_destroy(struct thread *thread);
/**
 * signal_process_init() - Initialize process-directed pending signals
 * @process: Process whose signal storage is being initialized.
 *
 * Context: Process allocation path; does not sleep.
 */
void signal_process_init(struct process *process);
/**
 * signal_process_destroy() - Drop process-directed pending signals
 * @process: Process being destroyed.
 *
 * Context: Process teardown; frees queued realtime entries.
 */
void signal_process_destroy(struct process *process);
/**
 * signal_thread_fork() - Initialize a fork child's inherited thread state
 * @child: New caller-locked child thread.
 * @parent: Caller-locked parent thread.
 *
 * Both process locks must be held. Pending signals are not inherited.
 */
void signal_thread_fork(struct thread *child, struct thread *parent);
/**
 * signal_thread_clone() - Initialize a clone child's thread signal state
 * @child: New caller-locked thread in the parent's process.
 * @parent: Existing thread in the same process.
 *
 * Context: The shared owning process lock must be held.
 */
void signal_thread_clone(struct thread *child, struct thread *parent);
/**
 * signal_process_fork() - Copy signal actions into a fork child process
 * @child: Caller-locked child process.
 * @parent: Caller-locked parent process.
 *
 * Context: Both process locks must be held. Pending signals are not copied.
 */
void signal_process_fork(struct process *child, struct process *parent);
/**
 * signal_process_exec() - Reset exec-sensitive signal state
 * @process: Process installing a new image.
 * @thread: Thread that survives the exec.
 *
 * Non-ignored dispositions return to default and the alternate stack is
 * disabled. This function acquires @process's lock.
 */
void signal_process_exec(struct process *process, struct thread *thread);
/**
 * signal_thread_detach_locked() - Retarget signals away from a departing thread
 * @process: Owning process.
 * @thread: Thread being detached from @process.
 *
 * Context: @process->lock held.
 */
void signal_thread_detach_locked(struct process *process,
				 struct thread *thread);
/**
 * signal_thread_mask_changed_locked() - Retarget after a mask change
 * @process: Owning process.
 * @thread: Thread whose mask changed.
 *
 * Context: @process->lock held.
 */
void signal_thread_mask_changed_locked(struct process *process,
				       struct thread *thread);

/**
 * signal_queue_process_locked() - Queue a process-directed signal
 * @process: Live destination process.
 * @signal: Signal number in the supported range.
 * @information: Optional source metadata copied into the queue.
 *
 * Standard signals coalesce; realtime signals are queued up to the configured
 * limit. A resumed stopped process returns a positive result.
 *
 * Context: @process->lock held.
 * Return: 0, a positive resume indication, SIGNAL_QUEUE_FULL, or -1.
 */
int signal_queue_process_locked(struct process *process, int signal,
				const struct signal_info *information);
/**
 * signal_queue_thread_locked() - Queue a signal for one eligible thread
 * @process: Live owning process.
 * @thread: Destination thread in @process.
 * @signal: Signal number in the supported range.
 * @information: Optional source metadata copied into the queue.
 *
 * Context: @process->lock held.
 * Return: As signal_queue_process_locked().
 */
int signal_queue_thread_locked(struct process *process,
			       struct thread *thread, int signal,
			       const struct signal_info *information);
/* Lookup and permission checks are performed by the signal-send helpers. */
/**
 * signal_send_process() - Queue a process-directed signal to one process
 * @pid: Target thread-group ID.
 * @signal: Valid Linux signal number; zero performs permission checking only.
 * @information: Sender metadata copied into the target pending queue.
 *
 * Context: Process context; serializes with the global process list.
 * Return: Zero, SIGNAL_QUEUE_FULL, SIGNAL_QUEUE_DENIED, or a negative lookup
 * error.
 */
int signal_send_process(int pid, int signal,
			const struct signal_info *information);
/**
 * signal_send_processes() - Queue a signal selected by kill-style PID rules
 * @selector: Positive PID, zero caller group, or negative process-group form.
 * @signal: Valid Linux signal number.
 * @information: Sender metadata copied to every selected target.
 *
 * Context: Process context; serializes with the global process list.
 * Return: Zero, queue/permission status, or a negative lookup error.
 */
int signal_send_processes(int selector, int signal,
			  const struct signal_info *information);
/**
 * signal_send_thread() - Queue a thread-directed signal
 * @thread_group: Required thread-group ID, or zero when not constrained.
 * @tid: Target thread ID.
 * @signal: Valid Linux signal number.
 * @information: Sender metadata copied into the thread pending queue.
 *
 * Context: Process context; serializes with the global process list.
 * Return: Zero, SIGNAL_QUEUE_FULL, SIGNAL_QUEUE_DENIED, or a negative lookup
 * error.
 */
int signal_send_thread(int thread_group, int tid, int signal,
		       const struct signal_info *information);

/**
 * signal_pending_unblocked() - Test whether a thread has deliverable work
 * @thread: Thread to inspect.
 *
 * Context: Any thread context; temporarily acquires the owning process lock.
 * Return: Nonzero for a pending unmasked signal, otherwise zero.
 */
int signal_pending_unblocked(struct thread *thread);
/**
 * signal_fatal_pending() - Test whether a thread has a fatal signal pending
 * @thread: Thread to inspect.
 *
 * Context: Any thread context. The process lock may already be held.
 * Return: Nonzero when default signal disposition would terminate the thread.
 */
int signal_fatal_pending(struct thread *thread);
/**
 * signal_mask_sanitize() - Remove unmaskable signals from a signal mask
 * @mask: Linux signal bit mask.
 *
 * Return: @mask without SIGKILL and SIGSTOP bits.
 */
uint64 signal_mask_sanitize(uint64 mask);
/**
 * signal_raise_current() - Queue a synchronous signal for the current thread
 * @signal: Valid signal number.
 * @code: Linux siginfo code describing the cause.
 *
 * Context: Current user thread; does not sleep.
 */
void signal_raise_current(int signal, int code);
/**
 * signal_force_fault() - Force a fault signal despite normal masking rules
 * @signal: Valid synchronous fault signal.
 * @code: Linux fault code.
 * @address: Faulting user virtual address.
 *
 * Context: Current user thread; does not sleep.
 */
void signal_force_fault(int signal, int code, uint64 address);
/**
 * signal_user_return() - Deliver pending signals before returning to user mode
 * @from_syscall: Nonzero when returning from a syscall eligible for restart.
 *
 * Context: Current user thread at trap return. May sleep for job-control stop
 * handling and may arrange a user signal frame or terminate the process.
 */
void signal_user_return(int from_syscall);

#endif
