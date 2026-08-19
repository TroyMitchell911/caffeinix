#include <cpu.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <mem_layout.h>
#include <mystring.h>
#include <process.h>
#include <random.h>
#include <scheduler.h>
#include <syscall.h>
#include <timer.h>
#include <vm.h>
#include <wait.h>

static int linux_timespec_to_ns(const struct linux_timespec *time,
				uint64 *nanoseconds)
{
	if (time->seconds < 0 || time->nanoseconds < 0 ||
	    time->nanoseconds >= (int64)NSEC_PER_SEC)
		return -LINUX_EINVAL;
	if ((uint64)time->seconds >
	    (~(uint64)0 - (uint64)time->nanoseconds) / NSEC_PER_SEC)
		return -LINUX_EINVAL;
	*nanoseconds = (uint64)time->seconds * NSEC_PER_SEC +
		       (uint64)time->nanoseconds;
	return 0;
}

static int linux_clock_now(int clock, uint64 *nanoseconds)
{
	switch (clock) {
	case LINUX_CLOCK_REALTIME:
		return ktime_get_realtime_ns(nanoseconds) < 0 ?
			-LINUX_ENODEV : 0;
	case LINUX_CLOCK_MONOTONIC:
		*nanoseconds = ktime_get_ns();
		return 0;
	case LINUX_CLOCK_BOOTTIME:
		*nanoseconds = ktime_get_boot_ns();
		return 0;
	default:
		return -LINUX_EINVAL;
	}
}

static int linux_copy_timespec(uint64 address, uint64 nanoseconds)
{
	struct linux_timespec time = {
		.seconds = nanoseconds / NSEC_PER_SEC,
		.nanoseconds = nanoseconds % NSEC_PER_SEC,
	};

	return copyout(cur_proc()->pagetable, address, (char *)&time,
		       sizeof(time)) < 0 ? -LINUX_EFAULT : 0;
}

uint64 sys_linux_clock_gettime(void)
{
	uint64 address, nanoseconds;
	int clock, result;

	argint(0, &clock);
	argaddr(1, &address);
	result = linux_clock_now(clock, &nanoseconds);
	return result < 0 ? result : linux_copy_timespec(address, nanoseconds);
}

uint64 sys_linux_clock_getres(void)
{
	uint64 address, resolution;
	int clock;

	argint(0, &clock);
	argaddr(1, &address);
	if (clock != LINUX_CLOCK_REALTIME &&
	    clock != LINUX_CLOCK_MONOTONIC &&
	    clock != LINUX_CLOCK_BOOTTIME)
		return -LINUX_EINVAL;
	if (!address)
		return 0;
	resolution = (NSEC_PER_SEC + timer_frequency() - 1) /
		     timer_frequency();
	return linux_copy_timespec(address, resolution);
}

static int linux_sleep_until(uint64 deadline, uint64 remaining_address,
			     int report_remaining)
{
	process_t process = cur_proc();
	uint64 now, remaining;
	int result;

	spinlock_acquire(&process->sleep_lock);
	result = wait_queue_sleep_interruptible_until(
		&process->sleep_wait, &process->sleep_lock, deadline);
	spinlock_release(&process->sleep_lock);
	if (result != WAIT_QUEUE_INTERRUPTED)
		return 0;
	if (report_remaining && remaining_address) {
		now = ktime_get_ticks();
		remaining = deadline > now ?
			ktime_ticks_to_ns(deadline - now, timer_frequency()) : 0;
		result = linux_copy_timespec(remaining_address, remaining);
		if (result < 0)
			return result;
	}
	return -LINUX_EINTR;
}

uint64 sys_linux_nanosleep(void)
{
	struct linux_timespec requested;
	process_t process = cur_proc();
	uint64 request_address, remaining_address;
	uint64 duration, delta, deadline, now;
	int result;

	argaddr(0, &request_address);
	argaddr(1, &remaining_address);
	if (!request_address ||
	    copyin(process->pagetable, (char *)&requested,
	           request_address, sizeof(requested)) < 0)
		return -LINUX_EFAULT;
	result = linux_timespec_to_ns(&requested, &duration);
	if (result < 0)
		return result;
	if (!duration)
		return 0;
	now = ktime_get_ticks();
	delta = ktime_ns_to_ticks(duration);
	deadline = now + delta;
	if (deadline < now)
		deadline = ~(uint64)0;
	return linux_sleep_until(deadline, remaining_address, 1);
}

uint64 sys_linux_clock_nanosleep(void)
{
	struct linux_timespec requested;
	process_t process = cur_proc();
	uint64 request_address, remaining_address;
	uint64 requested_ns, current_ns, delta, deadline, now;
	int clock, flags, result;

	argint(0, &clock);
	argint(1, &flags);
	argaddr(2, &request_address);
	argaddr(3, &remaining_address);
	if (flags & ~LINUX_TIMER_ABSTIME)
		return -LINUX_EINVAL;
	if (!request_address ||
	    copyin(process->pagetable, (char *)&requested,
	           request_address, sizeof(requested)) < 0)
		return -LINUX_EFAULT;
	result = linux_timespec_to_ns(&requested, &requested_ns);
	if (result < 0)
		return result;
	result = linux_clock_now(clock, &current_ns);
	if (result < 0)
		return result;
	if (flags & LINUX_TIMER_ABSTIME) {
		if (requested_ns <= current_ns)
			return 0;
		delta = ktime_ns_to_ticks(requested_ns - current_ns);
	} else {
		if (!requested_ns)
			return 0;
		delta = ktime_ns_to_ticks(requested_ns);
	}
	now = ktime_get_ticks();
	deadline = now + delta;
	if (deadline < now)
		deadline = ~(uint64)0;
	return linux_sleep_until(deadline, remaining_address,
				 !(flags & LINUX_TIMER_ABSTIME));
}

uint64 sys_linux_gettimeofday(void)
{
	struct linux_timezone timezone = { 0 };
	struct linux_timeval time;
	process_t process = cur_proc();
	uint64 time_address, timezone_address, nanoseconds;

	argaddr(0, &time_address);
	argaddr(1, &timezone_address);
	if (ktime_get_realtime_ns(&nanoseconds) < 0)
		return -LINUX_ENODEV;
	if (time_address) {
		time.seconds = nanoseconds / NSEC_PER_SEC;
		time.microseconds = nanoseconds % NSEC_PER_SEC / 1000;
		if (copyout(process->pagetable, time_address, (char *)&time,
		            sizeof(time)) < 0)
			return -LINUX_EFAULT;
	}
	if (timezone_address &&
	    copyout(process->pagetable, timezone_address,
	            (char *)&timezone, sizeof(timezone)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_exit_group(void)
{
	int status;

	argint(0, &status);
	process_thread_exit(status, 1);
	return 0;
}

uint64 sys_linux_exit(void)
{
	int status;

	argint(0, &status);
	process_thread_exit(status, 0);
	return 0;
}

uint64 sys_linux_set_tid_address(void)
{
	uint64 address;

	argaddr(0, &address);
	cur_thread()->clear_child_tid = address;
	return cur_thread()->tid;
}

uint64 sys_linux_prctl(void)
{
	process_t p = cur_proc();
	char name[MAXNAME];
	uint64 address;
	int option;

	argint(0, &option);
	argaddr(1, &address);
	if (option != LINUX_PR_GET_NAME)
		return -LINUX_EINVAL;
	memset(name, 0, sizeof(name));
	safe_strncpy(name, p->name, sizeof(name));
	if (copyout(p->pagetable, address, name, sizeof(name)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_getpid(void)
{
	return cur_proc()->pid;
}

uint64 sys_linux_getppid(void)
{
	process_t parent = cur_proc()->parent;

	return parent ? parent->pid : 0;
}

uint64 sys_linux_getuid(void)
{
	return 0;
}

uint64 sys_linux_geteuid(void)
{
	return 0;
}

uint64 sys_linux_getgid(void)
{
	return 0;
}

uint64 sys_linux_getegid(void)
{
	return 0;
}

uint64 sys_linux_gettid(void)
{
	return cur_thread()->tid;
}

uint64 sys_linux_setpgid(void)
{
	int pgid, pid;

	argint(0, &pid);
	argint(1, &pgid);
	return process_setpgid(pid, pgid);
}

uint64 sys_linux_getpgid(void)
{
	int pid;

	argint(0, &pid);
	return process_getpgid(pid);
}

uint64 sys_linux_getsid(void)
{
	int pid;

	argint(0, &pid);
	return process_getsid(pid);
}

uint64 sys_linux_setsid(void)
{
	return process_setsid();
}

uint64 sys_linux_getrandom(void)
{
	uint8 buffer[64];
	uint64 address, length, total = 0;
	int flags;

	argaddr(0, &address);
	argaddr(1, &length);
	argint(2, &flags);
	if (flags & ~(LINUX_GRND_NONBLOCK | LINUX_GRND_RANDOM |
		      LINUX_GRND_INSECURE) ||
	    (flags & LINUX_GRND_RANDOM && flags & LINUX_GRND_INSECURE))
		return -LINUX_EINVAL;
	if (length && address > (uint64)-1 - length)
		return -LINUX_EFAULT;
	while (total < length) {
		uint64 count = length - total;

		if (count > sizeof(buffer))
			count = sizeof(buffer);
		if (get_random_bytes(buffer, count) < 0)
			return total ? total : -LINUX_EAGAIN;
		if (copyout(cur_proc()->pagetable, address + total,
			    (char *)buffer, count) < 0) {
			memset(buffer, 0, sizeof(buffer));
			return -LINUX_EFAULT;
		}
		total += count;
	}
	memset(buffer, 0, sizeof(buffer));
	return total;
}

uint64 sys_linux_setpriority(void)
{
	int which, who, nice;

	argint(0, &which);
	argint(1, &who);
	argint(2, &nice);
	if (which != LINUX_PRIO_PROCESS)
		return -LINUX_EINVAL;
	if (nice < -20)
		nice = -20;
	if (nice > 19)
		nice = 19;
	return process_set_nice(who, nice);
}

uint64 sys_linux_getpriority(void)
{
	int nice, which, who;

	argint(0, &which);
	argint(1, &who);
	if (which != LINUX_PRIO_PROCESS)
		return -LINUX_EINVAL;
	if (process_get_nice(who, &nice))
		return -LINUX_ESRCH;
	/* Linux returns 20 - nice so a negative nice value is not an error. */
	return 20 - nice;
}

uint64 sys_linux_umask(void)
{
	process_t process = cur_proc();
	uint32 old_mask = process->umask;
	int mask;

	argint(0, &mask);
	process->umask = mask & 0777;
	return old_mask;
}

uint64 sys_linux_riscv_flush_icache(void)
{
	uint64 end, flags, start;

	argaddr(0, &start);
	argaddr(1, &end);
	argaddr(2, &flags);
	(void)start;
	(void)end;
	if (flags & ~LINUX_SYS_RISCV_FLUSH_ICACHE_ALL)
		return -LINUX_EINVAL;

	/*
	 * Until address spaces track stale instruction caches per hart,
	 * conservatively flush every online hart for both Linux modes.
	 */
	cpu_icache_flush_all();
	return 0;
}

uint64 sys_linux_clone(void)
{
	const uint64 thread_required =
		LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES |
		LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD;
	const uint64 thread_allowed =
		thread_required | LINUX_CLONE_SYSVSEM | LINUX_CLONE_SETTLS |
		LINUX_CLONE_PARENT_SETTID | LINUX_CLONE_CHILD_CLEARTID |
		LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_DETACHED;
	uint64 child_stack, child_tid, flags, parent_tid, tls;
	int pid;

	argaddr(0, &flags);
	argaddr(1, &child_stack);
	argaddr(2, &parent_tid);
	argaddr(3, &tls);
	argaddr(4, &child_tid);
	if (flags & LINUX_CLONE_THREAD) {
		if ((flags & thread_required) != thread_required ||
		    flags & ~thread_allowed ||
		    (flags & LINUX_CLONE_SIGNAL_MASK))
			return -LINUX_EINVAL;
		return process_clone_thread(flags, child_stack, parent_tid,
		                            tls, child_tid);
	}
	if ((flags & LINUX_CLONE_SIGNAL_MASK) != LINUX_SIGCHLD ||
	    flags & ~(LINUX_CLONE_SIGNAL_MASK | LINUX_CLONE_VM |
	              LINUX_CLONE_VFORK))
		return -LINUX_EINVAL;
	pid = process_fork(child_stack);
	return pid < 0 ? -LINUX_ENOMEM : pid;
}

uint64 sys_linux_wait4(void)
{
	uint64 status_address;
	int options, result, target;

	argint(0, &target);
	argaddr(1, &status_address);
	argint(2, &options);
	if (options & ~(LINUX_WNOHANG | LINUX_WUNTRACED |
	                LINUX_WCONTINUED))
		return -LINUX_EINVAL;
	result = process_wait(target, status_address, options);
	if (result == PROCESS_WAIT_FAULT)
		return -LINUX_EFAULT;
	if (result == PROCESS_WAIT_INTR)
		return -SIGNAL_RESTART_SYS;
	if (result < 0)
		return -LINUX_ECHILD;
	return result;
}
