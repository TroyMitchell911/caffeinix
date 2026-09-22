/*
 * Process, thread-group, job-control, and process-accounting interfaces.
 *
 * A process owns an address space, file table, credentials, signal actions,
 * and one or more threads. Process-table operations use the global wait lock
 * followed by the individual process lock; callers must not reverse that
 * order. Objects stay on the process list until they are reaped.
 *
 * Copyright (c) 2024 by ${TroyMitchell}, All Rights Reserved.
 */
#ifndef __CAFFEINIX_KERNEL_PROCESS_H
#define __CAFFEINIX_KERNEL_PROCESS_H

#include <thread.h>
#include <spinlock.h>
#include <riscv.h>
#include <file.h>
#include <sleeplock.h>
#include <vma.h>
#include <wait.h>
#include <signal.h>

struct tty;
struct process_vfork;

#define PROCESS_CMDLINE_MAX PGSIZE
#define PROCESS_GROUP_MAX   32

/* Credentials shared by every thread in a process. */
struct process_credentials {
	uint32 uid;
	uint32 euid;
	uint32 suid;
	uint32 fsuid;
	uint32 gid;
	uint32 egid;
	uint32 sgid;
	uint32 fsgid;
	uint32 group_count;
	uint32 groups[PROCESS_GROUP_MAX];
};

struct process_snapshot {
	char name[MAXNAME];
	int pid;
	int ppid;
	int pgid;
	int sid;
	int tty;
	int tty_pgid;
	char state;
	int nice;
	uint32 uid;
	uint32 euid;
	uint32 suid;
	uint32 fsuid;
	uint32 gid;
	uint32 egid;
	uint32 sgid;
	uint32 fsgid;
	uint32 group_count;
	uint32 groups[PROCESS_GROUP_MAX];
	uint32 threads;
	uint32 runnable_threads;
	uint32 blocked_threads;
	uint64 start_time_ns;
	uint64 user_time_ns;
	uint64 system_time_ns;
	uint64 children_user_time_ns;
	uint64 children_system_time_ns;
	uint64 virtual_size;
	uint64 resident_pages;
	uint64 signal_pending;
	uint64 signal_shared_pending;
	uint64 signal_blocked;
	uint64 signal_ignored;
	uint64 signal_caught;
};

struct process_system_snapshot {
	uint32 processes;
	uint32 running;
	uint32 blocked;
	int last_pid;
	uint64 total_forks;
	uint64 context_switches;
	uint64 idle_time_ns;
	uint64 user_time_ns;
	uint64 system_time_ns;
};

/* A process progresses from allocation through execution to reaping. */
typedef enum process_state{
	PROCESS_EMBRYO,
        PROCESS_LIVE,
        PROCESS_ZOMBIE,
}process_state_t;

#define PROCESS_WAIT_FAULT -2
#define PROCESS_WAIT_INTR  -3

struct process_signal_action {
	uint64 handler;
	uint64 flags;
	uint64 mask;
};

/*
 * Persistent state for one Linux-compatible thread group.
 *
 * @lock protects scalar process state and signal actions. @files_lock and
 * @mmap_lock protect their respective subsystems. The process-list lock is
 * external, and serializes parent/child and lookup operations.
 */
typedef struct process{
        struct spinlock lock;

        char name[MAXNAME];
        int pid;
	int pgid;
	int sid;
        process_state_t state;
        
        uint64 sz;
	uint64 brk;
	uint64 brk_start;
	uint64 mmap_top;
        pagedir_t pagetable;
	struct sleeplock mmap_lock;
	struct vma_set vmas;
	struct list mmap_tag;
	uint8 mmap_registered;
	struct vfs_path root;
	struct vfs_path cwd;
	struct process_credentials credentials;
	uint32 umask;
	void *cmdline;
	uint32 cmdline_length;
	uint64 start_time_ns;
	uint64 retired_user_time_ns;
	uint64 retired_system_time_ns;
	uint64 children_user_time_ns;
	uint64 children_system_time_ns;
	struct spinlock files_lock;
        file_t ofile[NOFILE];
	uint8 fd_flags[NOFILE];
        int exit_state;
	int group_exiting;
	int group_exit_state;
	int group_exit_signal;
	int group_exit_core;
	int execing;
	int did_exec;
	int live_threads;
	int stopped;
	int child_event;
	int child_event_signal;
	uint8 auto_reap;
	uint8 membarrier_private_expedited;
	uint8 vfork_mmap_transferred;
	struct process_vfork *vfork;
	struct wait_queue vfork_wait;
	uint64 real_timer_deadline;
	uint64 real_timer_interval;
	struct signal_pending *signal_pending;
	struct process_signal_action signal_actions[64];
	struct wait_queue signal_wait;
	struct spinlock sleep_lock;
	struct wait_queue sleep_wait;
	struct tty *controlling_tty;
        struct process *parent;
	uint8 adopted_by_init;
        struct wait_queue child_wait;
	struct wait_queue thread_reap_wait;
        int tnums;
        thread_t thread[PROC_MAXTHREAD];
        
        struct list all_tag;
}*process_t;

/**
 * process_init() - Initialize the global process table
 *
 * Context: Early boot, before user tasks exist; must run exactly once.
 */
void process_init(void);
/**
 * process_pagedir() - Build an address space for one process thread
 * @p: Live process that will own the page table.
 * @thread: Thread belonging to @p whose trap frame is mapped.
 *
 * Maps the trampoline, signal restorer, and @thread trap frame. The caller
 * owns the returned page table and must release it with process_freepagedir().
 *
 * Context: Process construction; may allocate memory.
 * Return: New page table, or %0 after validation or allocation failure.
 */
pagedir_t process_pagedir(process_t p, thread_t thread);
/**
 * process_freepagedir() - Tear down a process address space
 * @pgdir: Page table returned by process_pagedir().
 * @sz: Retained compatibility size argument; it is not used.
 *
 * Context: No thread may execute in @pgdir. Consumes @pgdir.
 */
void process_freepagedir(pagedir_t pgdir, uint64 sz);
/**
 * process_fork() - Create a child process from the current thread group
 * @child_stack: Optional user stack pointer, or zero to preserve the parent.
 *
 * Context: Current process context; may allocate and sleep.
 * Return: Child PID in the parent, or a negative Linux-compatible error.
 */
int process_fork(uint64 child_stack);
/**
 * process_vfork() - Create a child sharing the caller's address space
 * @child_stack: Optional replacement user stack pointer.
 *
 * The parent normally remains blocked until the child execs or exits.  A
 * killable wait interruption detaches the shared address space and lets the
 * parent return while the child continues independently.
 * Context: Current process; may allocate and sleep.
 * Return: Child PID or a negative Linux-compatible error.
 */
int process_vfork(uint64 child_stack);
/**
 * process_clone_thread() - Create a thread in the current process group
 * @flags: Supported Linux CLONE flag subset.
 * @child_stack: User stack pointer for the child.
 * @parent_tid: Optional parent user address for the new TID.
 * @tls: Thread-pointer value when CLONE_SETTLS is requested.
 * @child_tid: Optional child user address for the new TID.
 *
 * Context: Current process; may allocate and sleep.
 * Return: Child TID or a negative Linux-compatible error.
 */
int process_clone_thread(uint64 flags, uint64 child_stack,
			 uint64 parent_tid, uint64 tls, uint64 child_tid);
/**
 * process_thread_exit() - Finish the current thread or its whole group
 * @cause: Linux wait-status cause reported to the parent.
 * @group: Non-zero requests termination of every thread in the group.
 *
 * Context: Current thread context. Does not return to the exiting thread.
 */
void process_thread_exit(int cause, int group);
/**
 * process_signal_exit() - Apply a default fatal signal action
 * @signal: Linux signal number.
 * @core_dumped: Non-zero when the wait status records a core dump.
 *
 * Context: Current thread; does not return.
 */
void process_signal_exit(int signal, int core_dumped);
/**
 * process_signal_stop() - Stop the current process for a signal
 * @signal: Linux stop signal number.
 *
 * Context: Current thread; publishes a child event and yields execution.
 */
void process_signal_stop(int signal);
/**
 * process_auto_reap() - Reap an auto-reaped zombie process
 * @process: Non-NULL auto-reap zombie with only its final thread remaining.
 *
 * Context: Scheduler context after switching off the final thread, with
 * runqueue, thread, and process locks released. Acquires wait_lock and the
 * process lock internally; does not sleep. Frees the process and its final
 * thread, so the caller must not access either after return.
 */
void process_auto_reap(process_t process);
/**
 * process_group_exiting() - Read a thread group's exit request
 * @process: Target process.
 * @status: Storage for the Linux wait status when an exit is pending.
 *
 * Context: May be called in process context; acquires the process lock.
 * Return: Non-zero when group exit is pending, otherwise zero.
 */
int process_group_exiting(process_t process, int *status);
/**
 * process_exec_begin() - Serialize an exec transaction for a process
 * @process: Process whose image will be replaced.
 * @thread: Calling thread, which remains runnable during quiescing.
 *
 * Context: Process context; may signal and wait for sibling threads.
 * Return: Zero on ownership of exec serialization, or a negative error.
 */
int process_exec_begin(process_t process, thread_t thread);
/**
 * process_exec_quiesce() - Stop sibling threads for an exec transaction
 * @process: Process held by a successful process_exec_begin().
 * @thread: Calling thread which is excluded from the stop request.
 *
 * Context: Process context; may sleep waiting for sibling exits.
 * Return: Zero after all siblings quiesce, or a negative interruption error.
 */
int process_exec_quiesce(process_t process, thread_t thread);
/**
 * process_exec_end() - Finish an exec serialization transaction
 * @process: Process passed to process_exec_begin().
 * @committed: Non-zero after image commit; zero rolls the transaction back.
 *
 * Context: Process context after quiesce or begin failure cleanup.
 */
void process_exec_end(process_t process, int committed);
/**
 * process_vfork_exec() - Release vfork state after committing a new exec image
 * @process: Exec caller with its new page directory installed, or NULL.
 *
 * Context: Process context; may sleep on the vfork-state and mmap sleeplocks.
 * Serializes with parent detachment, clears the child's vfork link, and drops
 * its state reference. Normal completion restores the parent's trapframe
 * mapping and wakes its vfork waiter. Inconsistent live state panics.
 * Return: 1 when the parent owns the old shared page directory, so exec must
 * not free it; 0 when exec must free the old directory, including a detached
 * child that owns it. NULL or no vfork state also returns 0. This is an
 * ownership predicate, never a negative errno result.
 */
int process_vfork_exec(process_t process);
/**
 * process_thread_exit_requested() - Test a thread's exec-time exit request
 * @thread: Live thread whose request state is inspected, or NULL.
 * @status: Optional output, written only when an exit request is present.
 *
 * Context: Any context; uses atomic request state and does not sleep.
 * Return: Nonzero when @thread has an exit request, whether or not @status
 * is supplied; zero for NULL @thread or no request.
 */
int process_thread_exit_requested(thread_t thread, int *status);
/**
 * process_wait() - Reap or observe a child state change
 * @target: Linux PID selector.
 * @status_address: Optional user address for a wait status.
 * @usage_address: Optional user rusage address.
 * @options: Linux wait options.
 *
 * Context: Current process; may sleep and is signal interruptible.
 * Return: Child PID, zero for WNOHANG, or a negative error.
 */
int process_wait(int target, uint64 status_address, uint64 usage_address,
		 int options);
/**
 * process_setpgid() - Set a child or caller process group
 * @pid: Zero for the caller or a child PID.
 * @pgid: Zero for the selected PID or an existing group identifier.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Zero or a negative Linux-compatible permission/lookup error.
 */
int process_setpgid(int pid, int pgid);
/**
 * process_getpgid() - Return a visible process group identifier
 * @pid: Zero for the caller or a visible process ID.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Process group ID or a negative lookup error.
 */
int process_getpgid(int pid);
/**
 * process_getsid() - Return a visible process session identifier
 * @pid: Zero for the caller or a visible process ID.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Session ID or a negative lookup error.
 */
int process_getsid(int pid);
/**
 * process_setsid() - Create a session for the current process
 *
 * Context: Process context; takes the global process-list lock.
 * Return: New session ID or a negative permission error.
 */
int process_setsid(void);
/**
 * process_controlling_tty() - Obtain the current controlling terminal
 *
 * Context: Process context; takes the global process-list lock.
 * Return: The TTY pointer or %NULL when the caller has none. The result is
 * borrowed and must not outlive terminal teardown.
 */
struct tty *process_controlling_tty(void);
/**
 * process_tty_open() - Apply controlling-terminal open rules
 * @tty: Open terminal, not %NULL.
 * @no_ctty: Non-zero when the open requests O_NOCTTY behavior.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Zero or a VFS error.
 */
int process_tty_open(struct tty *tty, int no_ctty);
/**
 * process_tty_busy() - Test whether a terminal belongs to a live session
 * @tty: Terminal to inspect.
 *
 * Context: Any non-IRQ context; takes the global process-list lock.
 * Return: Non-zero when owned by a live process, otherwise zero.
 */
int process_tty_busy(struct tty *tty);
/**
 * process_tty_get_foreground() - Read a terminal foreground group
 * @tty: Terminal owned by the caller's session.
 * @pgid: Output storage for the foreground process group.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Zero or a VFS permission/error result.
 */
int process_tty_get_foreground(struct tty *tty, int *pgid);
/**
 * process_tty_set_foreground() - Change a terminal foreground group
 * @tty: Terminal owned by the caller's session.
 * @pgid: Existing process group in that session.
 *
 * Context: Process context; may enforce job-control signal policy.
 * Return: Zero or a VFS permission/error result.
 */
int process_tty_set_foreground(struct tty *tty, int pgid);
/**
 * process_tty_get_session() - Read a terminal controlling session
 * @tty: Terminal owned by the caller's session.
 * @sid: Output storage for its session ID.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Zero or a VFS permission/error result.
 */
int process_tty_get_session(struct tty *tty, int *sid);
/**
 * process_tty_check_read() - Apply background-read job-control policy
 * @tty: Terminal being read.
 *
 * Context: Process context; may stop or signal the caller.
 * Return: Zero, or a VFS error when the read must fail.
 */
int process_tty_check_read(struct tty *tty);
/**
 * process_tty_check_write() - Apply background-write job-control policy
 * @tty: Terminal being written.
 * @force: Non-zero applies the background-group SIGTTOU policy; zero skips it.
 *
 * Context: Process context; may stop or signal the caller.
 * Return: Zero, or a VFS error when the write must fail.
 */
int process_tty_check_write(struct tty *tty, int force);
/**
 * process_tty_signal_foreground() - Send a terminal signal to a group
 * @tty: Terminal that supplies the foreground group.
 * @signal: Valid Linux signal number.
 *
 * Context: Process or terminal input context; takes process-list locks.
 * Return: Zero when delivered or no foreground group exists, otherwise error.
 */
int process_tty_signal_foreground(struct tty *tty, int signal);
/**
 * process_set_nice() - Change a task's nice value
 * @pid: Thread ID to modify, or zero for the current task.
 * @nice: Requested Linux nice value in the supported range.
 *
 * Context: Process context; serializes with scheduler state.
 * Return: Zero or a negative Linux-compatible error.
 */
int process_set_nice(int pid, int nice);
/**
 * process_get_nice() - Read a thread scheduling niceness
 * @pid: Zero for the caller or a visible thread ID.
 * @nice: Output storage for the niceness value.
 *
 * Context: Process context; serializes with scheduler state.
 * Return: Zero or a negative Linux-compatible lookup error.
 */
int process_get_nice(int pid, int *nice);
/**
 * process_expire_timers() - Deliver expired ITIMER_REAL events
 * @now: Monotonic boot-time timestamp in nanoseconds.
 *
 * Context: Timer processing; takes the process-list and process locks.
 */
void process_expire_timers(uint64 now);
/**
 * process_task_exists() - Test for a live thread ID
 * @tid: Thread ID to find.
 *
 * Context: Any non-IRQ context; takes the global process-list lock.
 * Return: Non-zero when a live thread has @tid, otherwise zero.
 */
int process_task_exists(int tid);
/**
 * process_task_count() - Count live tasks and unreaped leaders
 *
 * Context: Any non-IRQ context; takes the global process-list lock.
 * Return: Saturated task count.
 */
uint32 process_task_count(void);
/**
 * process_snapshot_pid() - Copy one process state snapshot
 * @pid: Process ID to inspect.
 * @snapshot: Output state snapshot, not %NULL.
 * @cmdline: Optional output command-line buffer.
 * @cmdline_size: Capacity of @cmdline in bytes.
 * @cmdline_length: Optional output valid command-line byte count.
 *
 * Context: Process context; takes process-list and process locks.
 * Return: Zero or a negative lookup/argument error.
 */
int process_snapshot_pid(int pid, struct process_snapshot *snapshot,
			 char *cmdline, uint32 cmdline_size,
			 uint32 *cmdline_length);
/**
 * process_snapshot_pids() - Enumerate visible process IDs
 * @pids: Output array, or %NULL when @capacity is zero.
 * @capacity: Number of entries available in @pids.
 *
 * Context: Process context; takes the global process-list lock.
 * Return: Number of IDs copied, never exceeding @capacity.
 */
uint32 process_snapshot_pids(int *pids, uint32 capacity);
/**
 * process_snapshot_system() - Aggregate system process accounting
 * @snapshot: Output system snapshot, not %NULL.
 *
 * Context: Process context; takes process-list and scheduler state locks.
 */
void process_snapshot_system(struct process_system_snapshot *snapshot);
/**
 * process_set_cmdline() - Replace a process's stored command line
 * @process: Target process.
 * @cmdline: Page-allocated command-line storage transferred to @process.
 * @length: Valid byte count, no larger than PROCESS_CMDLINE_MAX.
 *
 * Context: Caller owns @cmdline until this call. It must remain valid only
 * until ownership transfers; the old stored allocation is released here.
 */
void process_set_cmdline(process_t process, void *cmdline,
			 uint32 length);
/**
 * process_credentials_get() - Copy current process credentials
 * @credentials: Output credential storage, not %NULL.
 *
 * Context: Current process context; takes its process lock.
 */
void process_credentials_get(struct process_credentials *credentials);
/**
 * process_umask_get() - Read the current creation mask
 *
 * Context: Current process context; takes its process lock.
 * Return: Current umask bits.
 */
uint32 process_umask_get(void);
/**
 * process_umask_set() - Replace the current creation mask
 * @mask: New mask; only permission bits are retained.
 *
 * Context: Current process context; takes its process lock.
 * Return: Previous umask bits.
 */
uint32 process_umask_set(uint32 mask);

/**
 * either_copyout() - Copy a kernel buffer to user or kernel memory
 * @user_dst: Non-zero when @dst is a current-process user address.
 * @dst: Destination address.
 * @src: Kernel source buffer, valid for @len bytes.
 * @len: Byte count.
 *
 * Context: Current process context. The user path may fault.
 * Return: Zero or a negative copy fault.
 */
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len);
/**
 * either_copyin() - Copy user or kernel memory into a kernel buffer
 * @dst: Kernel destination, valid for @len bytes.
 * @user_src: Non-zero when @src is a current-process user address.
 * @src: Source address.
 * @len: Byte count.
 *
 * Context: Current process context. The user path may fault.
 * Return: Zero or a negative copy fault.
 */
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
/**
 * userinit() - Create and schedule the initial user process
 *
 * Context: Boot after process_init() and VM initialization; runs once.
 */
void userinit(void);
#endif
