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

void signal_thread_init(struct thread *thread);
void signal_thread_destroy(struct thread *thread);
void signal_process_init(struct process *process);
void signal_process_destroy(struct process *process);
void signal_thread_fork(struct thread *child, struct thread *parent);
void signal_thread_clone(struct thread *child, struct thread *parent);
void signal_process_fork(struct process *child, struct process *parent);
void signal_process_exec(struct process *process, struct thread *thread);
void signal_thread_detach_locked(struct process *process,
				 struct thread *thread);
void signal_thread_mask_changed_locked(struct process *process,
				       struct thread *thread);

int signal_queue_process_locked(struct process *process, int signal,
				const struct signal_info *information);
int signal_queue_thread_locked(struct process *process,
			       struct thread *thread, int signal,
			       const struct signal_info *information);
int signal_send_process(int pid, int signal,
			const struct signal_info *information);
int signal_send_processes(int selector, int signal,
			  const struct signal_info *information);
int signal_send_thread(int thread_group, int tid, int signal,
		       const struct signal_info *information);

int signal_pending_unblocked(struct thread *thread);
uint64 signal_mask_sanitize(uint64 mask);
void signal_raise_current(int signal, int code);
void signal_force_fault(int signal, int code, uint64 address);
void signal_user_return(int from_syscall);

#endif
