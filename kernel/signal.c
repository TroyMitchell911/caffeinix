#include <debug.h>
#include <futex.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <mem_layout.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <signal.h>
#include <syscall.h>
#include <vm.h>
#include <wait.h>

#define SIGNAL_ALTSTACK_MIN 2048
#define SIGNAL_REALTIME_LIMIT 128
#define SIGNAL_UNMASKABLE \
	(SIGNAL_BIT(LINUX_SIGKILL) | SIGNAL_BIT(LINUX_SIGSTOP))

#define SIGNAL_BIT(signal) (1ULL << ((signal) - 1))

struct signal_queue_entry {
	struct list list;
	struct signal_info information;
};

uint64 signal_mask_sanitize(uint64 mask)
{
	return mask & ~SIGNAL_UNMASKABLE;
}

static int signal_valid(int signal)
{
	return signal >= 1 && signal <= SIGNAL_COUNT;
}

static int signal_default_ignored(int signal)
{
	return signal == LINUX_SIGCHLD || signal == LINUX_SIGURG ||
	       signal == LINUX_SIGWINCH;
}

static int signal_default_stops(int signal)
{
	return signal == LINUX_SIGSTOP || signal == LINUX_SIGTSTP ||
	       signal == LINUX_SIGTTIN || signal == LINUX_SIGTTOU;
}

static int signal_default_cores(int signal)
{
	return signal == LINUX_SIGQUIT || signal == LINUX_SIGILL ||
	       signal == LINUX_SIGTRAP || signal == LINUX_SIGABRT ||
	       signal == LINUX_SIGBUS || signal == LINUX_SIGFPE ||
	       signal == LINUX_SIGSEGV || signal == LINUX_SIGXCPU ||
	       signal == LINUX_SIGXFSZ || signal == LINUX_SIGSYS;
}

static void signal_pending_init(struct signal_pending *pending)
{
	memset(pending, 0, sizeof(*pending));
	list_init(&pending->realtime);
}

static void signal_pending_destroy(struct signal_pending *pending)
{
	while (pending->realtime.next != &pending->realtime) {
		struct signal_queue_entry *entry = list_entry(
			pending->realtime.next, struct signal_queue_entry, list);

		list_remove(&entry->list);
		free(entry);
	}
	signal_pending_init(pending);
}

static void signal_thread_reset(thread_t thread)
{
	thread->signal_mask = 0;
	thread->signal_altstack_sp = 0;
	thread->signal_altstack_size = 0;
	thread->signal_altstack_flags = LINUX_SS_DISABLE;
	thread->signal_saved_mask = 0;
	thread->signal_restore_mask = 0;
	thread->syscall_a0 = 0;
	thread->syscall_restart = 0;
	futex_restart_cancel(thread);
	thread->signal_sequence = 0;
	thread->process_signal_target = 0;
}

void signal_thread_init(thread_t thread)
{
	signal_pending_init(&thread->signal_pending);
	signal_thread_reset(thread);
}

void signal_thread_destroy(thread_t thread)
{
	signal_pending_destroy(&thread->signal_pending);
	signal_thread_reset(thread);
}

void signal_process_init(process_t process)
{
	signal_pending_init(process->signal_pending);
}

void signal_process_destroy(process_t process)
{
	signal_pending_destroy(process->signal_pending);
}

void signal_thread_fork(thread_t child, thread_t parent)
{
	if (!child || !parent || !child->home || !parent->home ||
	    !spinlock_holding(&child->home->lock) ||
	    !spinlock_holding(&parent->home->lock))
		PANIC("fork signal state unlocked");
	signal_thread_init(child);
	child->signal_mask = parent->signal_mask;
	child->signal_altstack_sp = parent->signal_altstack_sp;
	child->signal_altstack_size = parent->signal_altstack_size;
	child->signal_altstack_flags = parent->signal_altstack_flags;
}

void signal_thread_clone(thread_t child, thread_t parent)
{
	if (!child || !parent || child->home != parent->home ||
	    !spinlock_holding(&child->home->lock))
		PANIC("clone signal state unlocked");
	signal_thread_init(child);
	child->signal_mask = parent->signal_mask;
}

void signal_process_fork(process_t child, process_t parent)
{
	if (!child || !parent || !spinlock_holding(&child->lock) ||
	    !spinlock_holding(&parent->lock))
		PANIC("process signal state unlocked");
	memmove(child->signal_actions, parent->signal_actions,
	        sizeof(child->signal_actions));
}

void signal_process_exec(process_t process, thread_t thread)
{
	int signal;

	spinlock_acquire(&process->lock);
	for (signal = 1; signal <= SIGNAL_COUNT; signal++) {
		if (process->signal_actions[signal - 1].handler !=
		    LINUX_SIG_IGN)
			memset(&process->signal_actions[signal - 1], 0,
			       sizeof(process->signal_actions[signal - 1]));
	}
	thread->signal_altstack_sp = 0;
	thread->signal_altstack_size = 0;
	thread->signal_altstack_flags = LINUX_SS_DISABLE;
	thread->signal_restore_mask = 0;
	signal_thread_mask_changed_locked(process, thread);
	spinlock_release(&process->lock);
}

static void signal_pending_clear(struct signal_pending *pending, int signal)
{
	uint64 bit = SIGNAL_BIT(signal);
	list_t current, next;

	pending->bits &= ~bit;
	memset(&pending->information[signal - 1], 0,
	       sizeof(pending->information[signal - 1]));
	for (current = pending->realtime.next;
	     current != &pending->realtime; current = next) {
		struct signal_queue_entry *entry = list_entry(
			current, struct signal_queue_entry, list);

		next = current->next;
		if (entry->information.signal != signal)
			continue;
		list_remove(current);
		pending->realtime_count--;
		free(entry);
	}
}

static void signal_clear_locked(process_t process, int signal)
{
	int index;

	signal_pending_clear(process->signal_pending, signal);
	for (index = 0; index < PROC_MAXTHREAD; index++) {
		thread_t thread = process->thread[index];

		if (thread) {
			signal_pending_clear(&thread->signal_pending, signal);
			thread->process_signal_target &= ~SIGNAL_BIT(signal);
		}
	}
}

static int signal_thread_targetable_locked(thread_t thread, int signal,
					   thread_t exclude)
{
	return thread && thread != exclude &&
	       thread->state != THREAD_UNUSED &&
	       thread->state != THREAD_ALLOCATED &&
	       thread->state != THREAD_EXITED &&
	       ((SIGNAL_BIT(signal) & SIGNAL_UNMASKABLE) ||
	       !(thread->signal_mask & SIGNAL_BIT(signal)));
}

static int signal_process_generation_ignored_locked(process_t process,
					    int signal)
{
	uint64 handler = process->signal_actions[signal - 1].handler;
	int index;

	if (handler == LINUX_SIG_IGN)
		return 1;
	if (handler != LINUX_SIG_DFL || !signal_default_ignored(signal))
		return 0;
	for (index = 0; index < PROC_MAXTHREAD; index++) {
		if (signal_thread_targetable_locked(process->thread[index],
		                                    signal, 0))
			return 1;
	}
	return 0;
}

static int signal_thread_generation_ignored_locked(process_t process,
					   thread_t thread, int signal)
{
	uint64 handler = process->signal_actions[signal - 1].handler;

	if (handler == LINUX_SIG_IGN)
		return 1;
	return handler == LINUX_SIG_DFL && signal_default_ignored(signal) &&
	       !(thread->signal_mask & SIGNAL_BIT(signal));
}

static void signal_target_clear_locked(process_t process, int signal)
{
	uint64 bit = SIGNAL_BIT(signal);
	int index;

	for (index = 0; index < PROC_MAXTHREAD; index++) {
		thread_t thread = process->thread[index];

		if (thread)
			thread->process_signal_target &= ~bit;
	}
}

static void signal_target_wake_locked(thread_t thread)
{
	__atomic_add_fetch(&thread->signal_sequence, 1, __ATOMIC_RELEASE);
	wait_queue_interrupt_thread(thread);
	scheduler_kick(thread);
}

static void signal_process_retarget_locked(process_t process, int signal,
					   thread_t exclude)
{
	uint64 bit = SIGNAL_BIT(signal);
	thread_t target = 0;
	int index;

	if (!(process->signal_pending->bits & bit)) {
		signal_target_clear_locked(process, signal);
		return;
	}
	for (index = 0; index < PROC_MAXTHREAD; index++) {
		thread_t thread = process->thread[index];

		if (thread && (thread->process_signal_target & bit) &&
		    signal_thread_targetable_locked(thread, signal, exclude))
			return;
	}
	signal_target_clear_locked(process, signal);
	for (index = 0; index < PROC_MAXTHREAD; index++) {
		thread_t thread = process->thread[index];

		if (signal_thread_targetable_locked(thread, signal, exclude)) {
			target = thread;
			break;
		}
	}
	if (!target)
		return;
	target->process_signal_target |= bit;
	signal_target_wake_locked(target);
}

void signal_thread_mask_changed_locked(process_t process, thread_t thread)
{
	uint64 pending;
	int signal;

	if (!process || !thread || thread->home != process ||
	    !spinlock_holding(&process->lock))
		PANIC("signal mask change unlocked");
	pending = process->signal_pending->bits;
	for (signal = 1; pending; signal++, pending >>= 1) {
		if (pending & 1)
			signal_process_retarget_locked(process, signal, 0);
	}
}

void signal_thread_detach_locked(process_t process, thread_t thread)
{
	uint64 targeted;
	int signal;

	if (!process || !thread || thread->home != process ||
	    !spinlock_holding(&process->lock))
		PANIC("signal detach unlocked");
	targeted = thread->process_signal_target;
	thread->process_signal_target = 0;
	for (signal = 1; targeted; signal++, targeted >>= 1) {
		if (targeted & 1)
			signal_process_retarget_locked(process, signal, thread);
	}
}

static int signal_prepare_group_locked(process_t process, int signal)
{
	int resumed = 0;

	if (signal == LINUX_SIGCONT) {
		signal_clear_locked(process, LINUX_SIGSTOP);
		signal_clear_locked(process, LINUX_SIGTSTP);
		signal_clear_locked(process, LINUX_SIGTTIN);
		signal_clear_locked(process, LINUX_SIGTTOU);
		if (process->stopped) {
			process->stopped = 0;
			resumed = 1;
		}
		wait_queue_wake_all(&process->signal_wait);
	} else if (signal_default_stops(signal)) {
		signal_clear_locked(process, LINUX_SIGCONT);
	} else if (signal == LINUX_SIGKILL) {
		process->stopped = 0;
		wait_queue_wake_all(&process->signal_wait);
	}
	return resumed;
}

static void signal_wake_locked(process_t process, thread_t target,
			       int signal)
{
	wait_queue_wake_all(&process->signal_wait);
	if (!target) {
		signal_process_retarget_locked(process, signal, 0);
		return;
	}
	if (!(SIGNAL_BIT(signal) & SIGNAL_UNMASKABLE) &&
	    (target->signal_mask & SIGNAL_BIT(signal)))
		return;
	signal_target_wake_locked(target);
}

static int signal_store_pending(struct signal_pending *pending, int signal,
				const struct signal_info *information)
{
	struct signal_info generated = {
		.signal = signal,
	};
	struct signal_queue_entry *entry = 0;
	uint64 bit = SIGNAL_BIT(signal);

	if (signal >= LINUX_SIGRTMIN) {
		if (pending->realtime_count >= SIGNAL_REALTIME_LIMIT)
			return SIGNAL_QUEUE_FULL;
		entry = malloc(sizeof(*entry));
		if (!entry)
			return SIGNAL_QUEUE_FULL;
		entry->information = information ? *information : generated;
		entry->information.signal = signal;
		list_init(&entry->list);
		list_insert_before(&pending->realtime, &entry->list);
		pending->realtime_count++;
		pending->bits |= bit;
		return 0;
	}

	/* Standard signals coalesce and retain the first queued siginfo. */
	if (pending->bits & bit)
		return 0;
	pending->bits |= bit;
	pending->information[signal - 1] = information ?
		*information : generated;
	pending->information[signal - 1].signal = signal;
	return 0;
}

int signal_queue_process_locked(process_t process, int signal,
				const struct signal_info *information)
{
	int result, resumed;

	if (!process || !spinlock_holding(&process->lock) ||
	    !signal_valid(signal) || process->state != PROCESS_LIVE)
		return -1;
	resumed = signal_prepare_group_locked(process, signal);
	if (!signal_process_generation_ignored_locked(process, signal)) {
		result = signal_store_pending(process->signal_pending, signal,
		                              information);
		if (result < 0)
			return result;
		signal_wake_locked(process, 0, signal);
	}
	return resumed;
}

int signal_queue_thread_locked(process_t process, thread_t thread,
			       int signal,
			       const struct signal_info *information)
{
	int result, resumed;

	if (!process || !thread || !spinlock_holding(&process->lock) ||
	    thread->home != process || thread->state == THREAD_UNUSED ||
	    thread->state == THREAD_EXITED || !signal_valid(signal) ||
	    process->state != PROCESS_LIVE)
		return -1;
	resumed = signal_prepare_group_locked(process, signal);
	if (!signal_thread_generation_ignored_locked(process, thread, signal)) {
		result = signal_store_pending(&thread->signal_pending, signal,
		                              information);
		if (result < 0)
			return result;
		signal_wake_locked(process, thread, signal);
	}
	return resumed;
}

int signal_pending_unblocked(thread_t thread)
{
	process_t process;
	uint64 pending;

	if (!thread || !(process = thread->home))
		return 0;
	spinlock_acquire(&process->lock);
	pending = (thread->signal_pending.bits |
	           (process->signal_pending->bits &
	            thread->process_signal_target)) & ~thread->signal_mask;
	spinlock_release(&process->lock);
	return !!pending;
}

static int signal_take_set_locked(process_t process, thread_t thread,
				  uint64 set,
				  int honor_process_target,
				  struct signal_info *information)
{
	struct signal_pending *pending;
	struct signal_queue_entry *entry = 0;
	list_t current;
	uint64 available;
	int signal;

	available = process->signal_pending->bits;
	if (honor_process_target)
		available &= thread->process_signal_target;
	available = (thread->signal_pending.bits | available) & set;
	if (!available)
		return 0;
	for (signal = 1; !(available & SIGNAL_BIT(signal)); signal++)
		;
	pending = thread->signal_pending.bits & SIGNAL_BIT(signal) ?
		&thread->signal_pending : process->signal_pending;
	if (signal >= LINUX_SIGRTMIN) {
		for (current = pending->realtime.next;
		     current != &pending->realtime; current = current->next) {
			entry = list_entry(current, struct signal_queue_entry, list);
			if (entry->information.signal == signal)
				break;
			entry = 0;
		}
		if (!entry) {
			PANIC("missing realtime signal");
			return 0;
		}
		*information = entry->information;
		list_remove(current);
		pending->realtime_count--;
		free(entry);
		for (current = pending->realtime.next;
		     current != &pending->realtime; current = current->next) {
			entry = list_entry(current, struct signal_queue_entry, list);
			if (entry->information.signal == signal)
				return signal;
		}
		pending->bits &= ~SIGNAL_BIT(signal);
		if (pending == process->signal_pending)
			signal_target_clear_locked(process, signal);
		return signal;
	}
	pending->bits &= ~SIGNAL_BIT(signal);
	if (pending == process->signal_pending)
		signal_target_clear_locked(process, signal);
	*information = pending->information[signal - 1];
	memset(&pending->information[signal - 1], 0,
	       sizeof(pending->information[signal - 1]));
	return signal;
}

static int signal_take_unblocked_locked(
	process_t process, thread_t thread, struct signal_info *information,
	struct process_signal_action *action)
{
	int signal;

	signal = signal_take_set_locked(process, thread,
		~thread->signal_mask, 1, information);
	if (!signal)
		return 0;
	*action = process->signal_actions[signal - 1];
	return signal;
}

void signal_force_fault(int signal, int code, uint64 address)
{
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	struct signal_info information = {
		.signal = signal,
		.code = code,
		.address = address,
	};

	spinlock_acquire(&process->lock);
	thread->signal_mask &= ~SIGNAL_BIT(signal);
	if (process->signal_actions[signal - 1].handler == LINUX_SIG_IGN)
		memset(&process->signal_actions[signal - 1], 0,
		       sizeof(process->signal_actions[signal - 1]));
	(void)signal_queue_thread_locked(process, thread, signal,
	                                 &information);
	spinlock_release(&process->lock);
}

static void signal_regs_save(struct linux_user_regs *registers,
			     trapframe_t trapframe)
{
	registers->pc = trapframe->epc;
	registers->ra = trapframe->ra;
	registers->sp = trapframe->sp;
	registers->gp = trapframe->gp;
	registers->tp = trapframe->tp;
	registers->t0 = trapframe->t0;
	registers->t1 = trapframe->t1;
	registers->t2 = trapframe->t2;
	registers->s0 = trapframe->s0;
	registers->s1 = trapframe->s1;
	registers->a0 = trapframe->a0;
	registers->a1 = trapframe->a1;
	registers->a2 = trapframe->a2;
	registers->a3 = trapframe->a3;
	registers->a4 = trapframe->a4;
	registers->a5 = trapframe->a5;
	registers->a6 = trapframe->a6;
	registers->a7 = trapframe->a7;
	registers->s2 = trapframe->s2;
	registers->s3 = trapframe->s3;
	registers->s4 = trapframe->s4;
	registers->s5 = trapframe->s5;
	registers->s6 = trapframe->s6;
	registers->s7 = trapframe->s7;
	registers->s8 = trapframe->s8;
	registers->s9 = trapframe->s9;
	registers->s10 = trapframe->s10;
	registers->s11 = trapframe->s11;
	registers->t3 = trapframe->t3;
	registers->t4 = trapframe->t4;
	registers->t5 = trapframe->t5;
	registers->t6 = trapframe->t6;
}

static void signal_regs_restore(trapframe_t trapframe,
				const struct linux_user_regs *registers)
{
	trapframe->epc = registers->pc;
	trapframe->ra = registers->ra;
	trapframe->sp = registers->sp;
	trapframe->gp = registers->gp;
	trapframe->tp = registers->tp;
	trapframe->t0 = registers->t0;
	trapframe->t1 = registers->t1;
	trapframe->t2 = registers->t2;
	trapframe->s0 = registers->s0;
	trapframe->s1 = registers->s1;
	trapframe->a0 = registers->a0;
	trapframe->a1 = registers->a1;
	trapframe->a2 = registers->a2;
	trapframe->a3 = registers->a3;
	trapframe->a4 = registers->a4;
	trapframe->a5 = registers->a5;
	trapframe->a6 = registers->a6;
	trapframe->a7 = registers->a7;
	trapframe->s2 = registers->s2;
	trapframe->s3 = registers->s3;
	trapframe->s4 = registers->s4;
	trapframe->s5 = registers->s5;
	trapframe->s6 = registers->s6;
	trapframe->s7 = registers->s7;
	trapframe->s8 = registers->s8;
	trapframe->s9 = registers->s9;
	trapframe->s10 = registers->s10;
	trapframe->s11 = registers->s11;
	trapframe->t3 = registers->t3;
	trapframe->t4 = registers->t4;
	trapframe->t5 = registers->t5;
	trapframe->t6 = registers->t6;
}

static int signal_on_altstack(thread_t thread, uint64 stack_pointer)
{
	uint64 end;

	if (thread->signal_altstack_flags & LINUX_SS_DISABLE)
		return 0;
	end = thread->signal_altstack_sp + thread->signal_altstack_size;
	return end >= thread->signal_altstack_sp &&
	       stack_pointer >= thread->signal_altstack_sp &&
	       stack_pointer < end;
}

static void signal_altstack_save(thread_t thread, uint64 stack_pointer,
				 struct linux_sigaltstack *stack)
{
	stack->sp = thread->signal_altstack_sp;
	stack->size = thread->signal_altstack_size;
	stack->padding = 0;
	stack->flags = thread->signal_altstack_flags;
	if (signal_on_altstack(thread, stack_pointer))
		stack->flags |= LINUX_SS_ONSTACK;
}

static int signal_altstack_restore(thread_t thread,
				   const struct linux_sigaltstack *stack)
{
	uint32 flags = stack->flags & ~LINUX_SS_ONSTACK;

	if (flags == LINUX_SS_DISABLE) {
		thread->signal_altstack_sp = 0;
		thread->signal_altstack_size = 0;
		thread->signal_altstack_flags = LINUX_SS_DISABLE;
		return 0;
	}
	if (flags & ~LINUX_SS_AUTODISARM ||
	    stack->size < SIGNAL_ALTSTACK_MIN ||
	    stack->sp >= MAXVA || stack->size > MAXVA - stack->sp)
		return -1;
	thread->signal_altstack_sp = stack->sp;
	thread->signal_altstack_size = stack->size;
	thread->signal_altstack_flags = flags;
	return 0;
}

static void signal_info_export(struct linux_siginfo *destination,
			       const struct signal_info *source)
{
	memset(destination, 0, sizeof(*destination));
	destination->signal = source->signal;
	destination->error = source->error;
	destination->code = source->code;
	if (source->code == LINUX_SI_USER ||
	    source->code == LINUX_SI_TKILL) {
		destination->fields.kill.pid = source->sender_pid;
		destination->fields.kill.uid = source->sender_uid;
	} else if (source->signal == LINUX_SIGCHLD) {
		destination->fields.child.pid = source->sender_pid;
		destination->fields.child.uid = source->sender_uid;
		destination->fields.child.status = source->status;
	} else {
		destination->fields.fault.address = source->address;
	}
}

static int signal_frame_setup(thread_t thread,
			      const struct signal_info *information,
			      const struct process_signal_action *action,
			      uint64 old_mask)
{
	struct linux_rt_sigframe frame;
	trapframe_t trapframe = thread->trapframe;
	process_t process = thread->home;
	uint64 frame_address, stack_pointer = trapframe->sp;
	int use_altstack = 0;

	memset(&frame, 0, sizeof(frame));
	if ((action->flags & LINUX_SA_ONSTACK) &&
	    !(thread->signal_altstack_flags & LINUX_SS_DISABLE) &&
	    !signal_on_altstack(thread, stack_pointer)) {
		stack_pointer = thread->signal_altstack_sp +
		                thread->signal_altstack_size;
		use_altstack = 1;
	}
	if (stack_pointer < sizeof(frame))
		return -1;
	frame_address = (stack_pointer - sizeof(frame)) & ~15ULL;
	if (frame_address >= MAXVA)
		return -1;
	signal_info_export(&frame.info, information);
	signal_altstack_save(thread, trapframe->sp, &frame.context.stack);
	frame.context.signal_mask = old_mask;
	signal_regs_save(&frame.context.mcontext.regs, trapframe);
	memmove(frame.context.mcontext.fpregs.d.f, trapframe->f,
	        sizeof(trapframe->f));
	frame.context.mcontext.fpregs.d.fcsr = trapframe->fcsr;
	frame.context.mcontext.fpregs.ext.reserved = 0;
	frame.context.mcontext.fpregs.ext.header.magic = 0;
	frame.context.mcontext.fpregs.ext.header.size = 0;
	if (copyout(process->pagetable, frame_address, (char *)&frame,
	            sizeof(frame)) < 0)
		return -1;
	if (use_altstack &&
	    (thread->signal_altstack_flags & LINUX_SS_AUTODISARM)) {
		thread->signal_altstack_sp = 0;
		thread->signal_altstack_size = 0;
		thread->signal_altstack_flags = LINUX_SS_DISABLE;
	}
	trapframe->epc = action->handler;
	trapframe->ra = USER_SIGRETURN;
	trapframe->sp = frame_address;
	trapframe->a0 = information->signal;
	trapframe->a1 = frame_address +
		__builtin_offsetof(struct linux_rt_sigframe, info);
	trapframe->a2 = frame_address +
		__builtin_offsetof(struct linux_rt_sigframe, context);
	return 0;
}

static void signal_restart_syscall(thread_t thread, int restart,
				   int through_handler)
{
	trapframe_t trapframe = thread->trapframe;

	if (!thread->syscall_restart)
		return;
	thread->syscall_restart = 0;
	if (!restart) {
		trapframe->a0 = -LINUX_EINTR;
		futex_restart_cancel(thread);
		return;
	}
	trapframe->a0 = thread->syscall_a0;
	trapframe->epc -= 4;
	futex_restart_signal(thread, through_handler);
}

void signal_user_return(int from_syscall)
{
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	struct process_signal_action action;
	struct signal_info information;
	uint64 frame_mask, new_mask;
	int signal;

	for (;;) {
		spinlock_acquire(&process->lock);
		if (process->stopped &&
		    !(thread->signal_pending.bits & SIGNAL_BIT(LINUX_SIGKILL)) &&
		    !(process->signal_pending->bits &
		      SIGNAL_BIT(LINUX_SIGKILL))) {
			wait_queue_sleep(&process->signal_wait, &process->lock);
			spinlock_release(&process->lock);
			continue;
		}
		signal = signal_take_unblocked_locked(process, thread,
		                                      &information, &action);
		if (!signal) {
			if (thread->signal_restore_mask) {
				thread->signal_mask = thread->signal_saved_mask;
				thread->signal_restore_mask = 0;
				signal_thread_mask_changed_locked(process, thread);
			}
			spinlock_release(&process->lock);
			if (from_syscall)
				signal_restart_syscall(thread, 1, 0);
			return;
		}
		if (action.handler == LINUX_SIG_IGN ||
		    (action.handler == LINUX_SIG_DFL &&
		     signal_default_ignored(signal))) {
			spinlock_release(&process->lock);
			continue;
		}
		if (action.handler == LINUX_SIG_DFL) {
			spinlock_release(&process->lock);
			if (signal == LINUX_SIGCONT)
				continue;
			if (signal_default_stops(signal)) {
				process_signal_stop(signal);
				continue;
			}
			process_signal_exit(signal,
			                    signal_default_cores(signal));
		}
		frame_mask = thread->signal_restore_mask ?
			thread->signal_saved_mask : thread->signal_mask;
		thread->signal_restore_mask = 0;
		new_mask = thread->signal_mask | action.mask;
		if (!(action.flags & LINUX_SA_NODEFER))
			new_mask |= SIGNAL_BIT(signal);
		thread->signal_mask = signal_mask_sanitize(new_mask);
		signal_thread_mask_changed_locked(process, thread);
		if (action.flags & LINUX_SA_RESETHAND)
			memset(&process->signal_actions[signal - 1], 0,
			       sizeof(process->signal_actions[signal - 1]));
		spinlock_release(&process->lock);
		if (from_syscall)
			signal_restart_syscall(
				thread, !!(action.flags & LINUX_SA_RESTART), 1);
		if (signal_frame_setup(thread, &information, &action,
		                       frame_mask) < 0)
			process_signal_exit(LINUX_SIGSEGV, 1);
		return;
	}
}

uint64 sys_linux_rt_sigreturn(void)
{
	struct linux_rt_sigframe frame;
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	trapframe_t trapframe = thread->trapframe;
	uint64 address = trapframe->sp;

	if (address & 15 || address >= MAXVA ||
	    sizeof(frame) > MAXVA - address ||
	    copyin(process->pagetable, (char *)&frame, address,
	           sizeof(frame)) < 0 ||
	    frame.context.mcontext.fpregs.ext.reserved ||
	    frame.context.mcontext.fpregs.ext.header.magic ||
	    frame.context.mcontext.fpregs.ext.header.size ||
	    frame.context.mcontext.regs.pc >= MAXVA ||
	    frame.context.mcontext.regs.sp >= MAXVA ||
	    signal_altstack_restore(thread, &frame.context.stack) < 0) {
		signal_force_fault(LINUX_SIGSEGV, LINUX_SEGV_MAPERR, address);
		return 0;
	}
	signal_regs_restore(trapframe, &frame.context.mcontext.regs);
	memmove(trapframe->f, frame.context.mcontext.fpregs.d.f,
	        sizeof(trapframe->f));
	trapframe->fcsr = frame.context.mcontext.fpregs.d.fcsr;
	spinlock_acquire(&process->lock);
	thread->signal_mask = signal_mask_sanitize(
		frame.context.signal_mask);
	thread->signal_restore_mask = 0;
	signal_thread_mask_changed_locked(process, thread);
	spinlock_release(&process->lock);
	futex_restart_sigreturn(thread);
	return trapframe->a0;
}

uint64 sys_linux_rt_sigaction(void)
{
	const uint64 supported_flags =
		LINUX_SA_NOCLDSTOP | LINUX_SA_NOCLDWAIT |
		LINUX_SA_SIGINFO |
		LINUX_SA_ONSTACK | LINUX_SA_RESTART |
		LINUX_SA_NODEFER | LINUX_SA_RESETHAND;
	struct linux_sigaction requested, old;
	process_t process = cur_proc();
	uint64 action_address, old_action_address, sigset_size;
	int signal;

	argint(0, &signal);
	argaddr(1, &action_address);
	argaddr(2, &old_action_address);
	argaddr(3, &sigset_size);
	if (!signal_valid(signal) || sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (action_address &&
	    copyin(process->pagetable, (char *)&requested, action_address,
	           sizeof(requested)) < 0)
		return -LINUX_EFAULT;
	/* musl widens its signed int sa_flags member to this UAPI word. */
	if (action_address)
		requested.flags = (uint32)requested.flags;
	if (action_address &&
	    (signal == LINUX_SIGKILL || signal == LINUX_SIGSTOP ||
	     requested.flags & ~supported_flags))
		return -LINUX_EINVAL;
	spinlock_acquire(&process->lock);
	memmove(&old, &process->signal_actions[signal - 1], sizeof(old));
	if (action_address) {
		requested.mask = signal_mask_sanitize(requested.mask);
		memmove(&process->signal_actions[signal - 1], &requested,
		        sizeof(requested));
		if (requested.handler == LINUX_SIG_IGN ||
		    (requested.handler == LINUX_SIG_DFL &&
		     signal_default_ignored(signal)))
			signal_clear_locked(process, signal);
	}
	spinlock_release(&process->lock);
	if (old_action_address &&
	    copyout(process->pagetable, old_action_address, (char *)&old,
	            sizeof(old)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_rt_sigprocmask(void)
{
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 mask, mask_address, old_mask_address, old_mask, sigset_size;
	int how;

	argint(0, &how);
	argaddr(1, &mask_address);
	argaddr(2, &old_mask_address);
	argaddr(3, &sigset_size);
	if (sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (mask_address &&
	    copyin(process->pagetable, (char *)&mask, mask_address,
	           sizeof(mask)) < 0)
		return -LINUX_EFAULT;
	spinlock_acquire(&process->lock);
	old_mask = thread->signal_mask;
	if (mask_address) {
		mask = signal_mask_sanitize(mask);
		if (how == LINUX_SIG_BLOCK)
			thread->signal_mask |= mask;
		else if (how == LINUX_SIG_UNBLOCK)
			thread->signal_mask &= ~mask;
		else if (how == LINUX_SIG_SETMASK)
			thread->signal_mask = mask;
		else {
			spinlock_release(&process->lock);
			return -LINUX_EINVAL;
		}
		signal_thread_mask_changed_locked(process, thread);
	}
	spinlock_release(&process->lock);
	if (old_mask_address &&
	    copyout(process->pagetable, old_mask_address, (char *)&old_mask,
	            sizeof(old_mask)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_rt_sigpending(void)
{
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 address, pending, sigset_size;

	argaddr(0, &address);
	argaddr(1, &sigset_size);
	if (sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	spinlock_acquire(&process->lock);
	pending = (process->signal_pending->bits |
	           thread->signal_pending.bits) & thread->signal_mask;
	spinlock_release(&process->lock);
	if (copyout(process->pagetable, address, (char *)&pending,
	            sizeof(pending)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

static int signal_send_syscall(int thread_group, int tid, int signal,
			       int thread_directed)
{
	struct signal_info information = {
		.signal = signal,
		.code = thread_directed ? LINUX_SI_TKILL : LINUX_SI_USER,
		.sender_pid = cur_proc()->pid,
		.sender_uid = 0,
	};
	int result;

	if (signal < 0 || signal > SIGNAL_COUNT)
		return -LINUX_EINVAL;
	if (thread_directed)
		result = signal_send_thread(thread_group, tid, signal,
		                            &information);
	else
		result = signal_send_process(tid, signal, &information);
	if (result == SIGNAL_QUEUE_FULL)
		return -LINUX_EAGAIN;
	return result < 0 ? -LINUX_ESRCH : 0;
}

uint64 sys_linux_kill(void)
{
	int pid, signal;

	argint(0, &pid);
	argint(1, &signal);
	if (pid <= 0)
		return -LINUX_EINVAL;
	return signal_send_syscall(0, pid, signal, 0);
}

uint64 sys_linux_tkill(void)
{
	int tid, signal;

	argint(0, &tid);
	argint(1, &signal);
	if (tid <= 0)
		return -LINUX_EINVAL;
	return signal_send_syscall(0, tid, signal, 1);
}

uint64 sys_linux_tgkill(void)
{
	int thread_group, tid, signal;

	argint(0, &thread_group);
	argint(1, &tid);
	argint(2, &signal);
	if (thread_group <= 0 || tid <= 0)
		return -LINUX_EINVAL;
	return signal_send_syscall(thread_group, tid, signal, 1);
}

uint64 sys_linux_sigaltstack(void)
{
	struct linux_sigaltstack requested, old;
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 address, old_address;

	argaddr(0, &address);
	argaddr(1, &old_address);
	if (address &&
	    copyin(process->pagetable, (char *)&requested, address,
	           sizeof(requested)) < 0)
		return -LINUX_EFAULT;
	spinlock_acquire(&process->lock);
	signal_altstack_save(thread, thread->trapframe->sp, &old);
	if (address) {
		if (signal_on_altstack(thread, thread->trapframe->sp)) {
			spinlock_release(&process->lock);
			return -LINUX_EPERM;
		}
		if (requested.flags == LINUX_SS_DISABLE) {
			thread->signal_altstack_sp = 0;
			thread->signal_altstack_size = 0;
			thread->signal_altstack_flags = LINUX_SS_DISABLE;
		} else if (requested.flags & ~LINUX_SS_AUTODISARM) {
			spinlock_release(&process->lock);
			return -LINUX_EINVAL;
		} else if (requested.size < SIGNAL_ALTSTACK_MIN) {
			spinlock_release(&process->lock);
			return -LINUX_ENOMEM;
		} else if (requested.sp >= MAXVA ||
		           requested.size > MAXVA - requested.sp) {
			spinlock_release(&process->lock);
			return -LINUX_EINVAL;
		} else {
			thread->signal_altstack_sp = requested.sp;
			thread->signal_altstack_size = requested.size;
			thread->signal_altstack_flags = requested.flags;
		}
	}
	spinlock_release(&process->lock);
	if (old_address &&
	    copyout(process->pagetable, old_address, (char *)&old,
	            sizeof(old)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_rt_sigsuspend(void)
{
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 address, mask, sigset_size;

	argaddr(0, &address);
	argaddr(1, &sigset_size);
	if (sigset_size != LINUX_SIGSET_SIZE ||
	    copyin(process->pagetable, (char *)&mask, address,
	           sizeof(mask)) < 0)
		return sigset_size != LINUX_SIGSET_SIZE ?
			-LINUX_EINVAL : -LINUX_EFAULT;
	spinlock_acquire(&process->lock);
	thread->signal_saved_mask = thread->signal_mask;
	thread->signal_restore_mask = 1;
	thread->signal_mask = signal_mask_sanitize(mask);
	signal_thread_mask_changed_locked(process, thread);
	while (!((thread->signal_pending.bits |
	          (process->signal_pending->bits &
	           thread->process_signal_target)) &
	         ~thread->signal_mask) &&
	       !process->group_exiting)
		wait_queue_sleep(&process->signal_wait, &process->lock);
	spinlock_release(&process->lock);
	return -LINUX_EINTR;
}

static int signal_timeout_ms(const struct linux_timespec *time,
			     uint64 *milliseconds)
{
	if (time->seconds < 0 || time->nanoseconds < 0 ||
	    time->nanoseconds >= 1000000000)
		return -1;
	if ((uint64)time->seconds > (~(uint64)0 - 999999999) / 1000) {
		*milliseconds = ~(uint64)0;
		return 0;
	}
	*milliseconds = time->seconds * 1000ULL +
		(time->nanoseconds + 999999) / 1000000;
	return 0;
}

uint64 sys_linux_rt_sigtimedwait(void)
{
	struct linux_timespec timeout;
	struct linux_siginfo exported;
	struct signal_info information;
	process_t process = cur_proc();
	thread_t thread = cur_thread();
	uint64 set_address, info_address, timeout_address, sigset_size;
	uint64 set, timeout_ms = 0, start = 0, remaining;
	int result, signal;

	argaddr(0, &set_address);
	argaddr(1, &info_address);
	argaddr(2, &timeout_address);
	argaddr(3, &sigset_size);
	if (sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (copyin(process->pagetable, (char *)&set, set_address,
	           sizeof(set)) < 0)
		return -LINUX_EFAULT;
	set = signal_mask_sanitize(set);
	if (timeout_address) {
		if (copyin(process->pagetable, (char *)&timeout,
		           timeout_address, sizeof(timeout)) < 0)
			return -LINUX_EFAULT;
		if (signal_timeout_ms(&timeout, &timeout_ms) < 0)
			return -LINUX_EINVAL;
		start = ktime_get_ms();
	}
	spinlock_acquire(&process->lock);
	for (;;) {
		signal = signal_take_set_locked(process, thread, set, 0,
		                                &information);
		if (signal)
			break;
		if ((process->signal_pending->bits |
		     thread->signal_pending.bits) & ~thread->signal_mask) {
			spinlock_release(&process->lock);
			return -LINUX_EINTR;
		}
		if (timeout_address) {
			uint64 elapsed = ktime_get_ms() - start;

			if (elapsed >= timeout_ms) {
				spinlock_release(&process->lock);
				return -LINUX_EAGAIN;
			}
			remaining = timeout_ms - elapsed;
			result = wait_queue_sleep_timeout(
				&process->signal_wait, &process->lock, remaining);
			if (result == WAIT_QUEUE_TIMEOUT)
				continue;
		} else {
			wait_queue_sleep(&process->signal_wait,
			                 &process->lock);
		}
	}
	spinlock_release(&process->lock);
	if (info_address) {
		signal_info_export(&exported, &information);
		if (copyout(process->pagetable, info_address,
		            (char *)&exported, sizeof(exported)) < 0)
			return -LINUX_EFAULT;
	}
	return signal;
}
