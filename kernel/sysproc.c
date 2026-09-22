/*
 * Linux process, identity, time, and scheduler system-call entry points.
 *
 * Each handler decodes RISC-V arguments from the current trap frame and
 * returns either a Linux result or a negative Linux errno. State ownership
 * remains in the process, scheduler, signal, timer, and VM subsystems.
 */
#include <cpu.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <loadavg.h>
#include <mem_layout.h>
#include <mystring.h>
#include <process.h>
#include <palloc.h>
#include <random.h>
#include <scheduler.h>
#include <syscall.h>
#include <timer.h>
#include <vm.h>
#include <wait.h>

/* Validate a Linux timespec and convert its normalized value to nanoseconds. */
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

/* Convert a validated timeval to timer ticks without overflow. */
static int linux_timeval_to_ticks(const struct linux_timeval *time,
				  uint64 *ticks)
{
	uint64 nanoseconds;

	if (time->seconds < 0 || time->microseconds < 0 ||
	    time->microseconds >= 1000000)
		return -LINUX_EINVAL;
	if ((uint64)time->seconds >
	    (~(uint64)0 - (uint64)time->microseconds * 1000) /
	    NSEC_PER_SEC)
		return -LINUX_EINVAL;
	nanoseconds = (uint64)time->seconds * NSEC_PER_SEC +
		      (uint64)time->microseconds * 1000;
	*ticks = ktime_ns_to_ticks(nanoseconds);
	return 0;
}

/* Serialize an internal tick duration using Linux timeval units. */
static void linux_ticks_to_timeval(uint64 ticks,
				   struct linux_timeval *time)
{
	uint64 nanoseconds = ktime_ticks_to_ns(ticks, timer_frequency());

	time->seconds = nanoseconds / NSEC_PER_SEC;
	time->microseconds = nanoseconds % NSEC_PER_SEC / 1000;
}

/* Snapshot the real timer while the target process lock is held. */
static void linux_getitimer_locked(process_t process,
				   struct linux_itimerval *timer,
				   uint64 now)
{
	uint64 remaining = process->real_timer_deadline > now ?
		process->real_timer_deadline - now : 0;

	linux_ticks_to_timeval(process->real_timer_interval,
			       &timer->interval);
	linux_ticks_to_timeval(remaining, &timer->value);
}

/* Read a supported Linux clock in nanoseconds. */
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

/* Copy a nanosecond duration to a current-process Linux timespec. */
static int linux_copy_timespec(uint64 address, uint64 nanoseconds)
{
	struct linux_timespec time = {
		.seconds = nanoseconds / NSEC_PER_SEC,
		.nanoseconds = nanoseconds % NSEC_PER_SEC,
	};

	return copyout(cur_proc()->pagetable, address, (char *)&time,
		       sizeof(time)) < 0 ? -LINUX_EFAULT : 0;
}

/**
 * sys_linux_clock_gettime() - Implement Linux clock_gettime
 *
 * Decodes Linux RISC-V register arguments and copies a timespec to user memory.
 * Context: User syscall context; may fault on the user copy.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_clock_gettime(void)
{
	uint64 address, nanoseconds;
	int clock, result;

	argint(0, &clock);
	argaddr(1, &address);
	result = linux_clock_now(clock, &nanoseconds);
	return result < 0 ? result : linux_copy_timespec(address, nanoseconds);
}

/**
 * sys_linux_clock_getres() - Implement Linux clock_getres
 *
 * Decodes Linux RISC-V register arguments and optionally writes user memory.
 * Context: User syscall context; may fault on the user copy.
 * Return: Zero or a negative Linux errno.
 */
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

/* Sleep interruptibly until a deadline, optionally reporting remaining time. */
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

/**
 * sys_linux_nanosleep() - Sleep for a Linux timespec interval
 *
 * Decodes Linux RISC-V register arguments and may report remaining time.
 * Context: User syscall context; sleeps interruptibly.
 * Return: Zero, or a negative Linux errno including interruption.
 */
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

/**
 * sys_linux_getitimer() - Read the calling process real-time interval timer
 *
 * Decodes Linux RISC-V register arguments and copies timer state to user
 * memory.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_getitimer(void)
{
	struct linux_itimerval timer;
	process_t process = cur_proc();
	uint64 address;
	int which;

	argint(0, &which);
	argaddr(1, &address);
	if (which != LINUX_ITIMER_REAL)
		return -LINUX_EINVAL;
	spinlock_acquire(&process->lock);
	linux_getitimer_locked(process, &timer, ktime_get_ticks());
	spinlock_release(&process->lock);
	if (copyout(process->pagetable, address, (char *)&timer,
	            sizeof(timer)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

/**
 * sys_linux_setitimer() - Set the calling process real-time interval timer
 *
 * Decodes Linux RISC-V register arguments and may copy old state to user
 * memory.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_setitimer(void)
{
	struct linux_itimerval requested = { 0 }, previous;
	process_t process = cur_proc();
	uint64 requested_address, previous_address;
	uint64 interval, value, now, deadline;
	int result, which;

	argint(0, &which);
	argaddr(1, &requested_address);
	argaddr(2, &previous_address);
	if (which != LINUX_ITIMER_REAL)
		return -LINUX_EINVAL;
	if (requested_address &&
	    copyin(process->pagetable, (char *)&requested,
	           requested_address, sizeof(requested)) < 0)
		return -LINUX_EFAULT;
	result = linux_timeval_to_ticks(&requested.interval, &interval);
	if (result < 0)
		return result;
	result = linux_timeval_to_ticks(&requested.value, &value);
	if (result < 0)
		return result;
	now = ktime_get_ticks();
	deadline = now + value;
	if (value && deadline < now)
		deadline = ~(uint64)0;
	spinlock_acquire(&process->lock);
	linux_getitimer_locked(process, &previous, now);
	process->real_timer_interval = interval;
	process->real_timer_deadline = value ? deadline : 0;
	spinlock_release(&process->lock);
	if (previous_address &&
	    copyout(process->pagetable, previous_address, (char *)&previous,
	            sizeof(previous)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

/**
 * sys_linux_clock_nanosleep() - Sleep against a supported Linux clock
 *
 * Decodes Linux RISC-V register arguments for relative or absolute waits.
 * Context: User syscall context; sleeps interruptibly.
 * Return: Zero, or a negative Linux errno including interruption.
 */
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

/**
 * sys_linux_gettimeofday() - Copy current wall-clock time to user memory
 *
 * Decodes Linux RISC-V register arguments; timezone output is unsupported.
 * Context: User syscall context; may fault on the user copy.
 * Return: Zero or a negative Linux errno.
 */
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

/**
 * sys_linux_uname() - Copy the fixed Caffeinix Linux identity
 *
 * Decodes the Linux RISC-V user buffer argument.
 * Context: User syscall context; may fault on the user copy.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_uname(void)
{
	struct linux_utsname name;
	uint64 address;

	argaddr(0, &address);
	memset(&name, 0, sizeof(name));
	safe_strncpy(name.sysname, "Caffeinix", sizeof(name.sysname));
	safe_strncpy(name.nodename, "caffeinix", sizeof(name.nodename));
	safe_strncpy(name.release, "0.1.0", sizeof(name.release));
	safe_strncpy(name.version, "Caffeinix 0.1.0",
	             sizeof(name.version));
	safe_strncpy(name.machine, "riscv64", sizeof(name.machine));
	safe_strncpy(name.domainname, "(none)", sizeof(name.domainname));
	if (copyout(cur_proc()->pagetable, address, (char *)&name,
	            sizeof(name)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

/**
 * sys_linux_sysinfo() - Report aggregate system information
 *
 * Decodes the Linux RISC-V user buffer argument and snapshots kernel state.
 * Context: User syscall context; may fault on the user copy.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_sysinfo(void)
{
	struct linux_sysinfo information;
	uint64 address, free_bytes;
	uint32 count, index, loads[3];

	argaddr(0, &address);
	memset(&information, 0, sizeof(information));
	information.uptime = ktime_get_boot_ns() / NSEC_PER_SEC;
	loadavg_get(loads);
	for (index = 0; index < NELEM(loads); index++)
		information.loads[index] = (uint64)loads[index] <<
			(LINUX_SI_LOAD_SHIFT - LOADAVG_FIXED_SHIFT);
	information.totalram = palloc_usable_bytes();
	free_bytes = palloc_free_pages();
	information.freeram = free_bytes > ~(uint64)0 / PGSIZE ?
		~(uint64)0 : free_bytes * PGSIZE;
	count = process_task_count();
	information.procs = count > 0xffff ? 0xffff : count;
	information.mem_unit = 1;
	if (copyout(cur_proc()->pagetable, address, (char *)&information,
	            sizeof(information)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

/**
 * sys_linux_sched_getaffinity() - Report the scheduler CPU mask
 *
 * Decodes Linux RISC-V register arguments and writes the supported affinity.
 * Context: User syscall context; may fault on the user copy.
 * Return: Byte count or a negative Linux errno.
 */
uint64 sys_linux_sched_getaffinity(void)
{
	process_t process = cur_proc();
	uint8 *mask;
	uint64 size, address, required;
	int count, cpu, pid;

	argint(0, &pid);
	argaddr(1, &size);
	argaddr(2, &address);
	if (pid < 0 || (pid && !process_task_exists(pid)))
		return -LINUX_ESRCH;
	count = cpu_count();
	required = ((uint64)count + 63) / 64 * sizeof(uint64);
	if (size < required)
		return -LINUX_EINVAL;
	mask = calloc(required, 1);
	if (!mask)
		return -LINUX_ENOMEM;
	for (cpu = 0; cpu < count; cpu++)
		mask[cpu / 8] |= 1U << (cpu % 8);
	if (copyout(process->pagetable, address, (char *)mask,
	            required) < 0) {
		free(mask);
		return -LINUX_EFAULT;
	}
	free(mask);
	return required;
}

/**
 * sys_linux_exit_group() - Terminate the caller's complete thread group
 *
 * Decodes the Linux RISC-V wait-status argument.
 * Context: User syscall context; releases process resources and never returns.
 */
uint64 sys_linux_exit_group(void)
{
	int status;

	argint(0, &status);
	process_thread_exit(status, 1);
	return 0;
}

/**
 * sys_linux_exit() - Terminate only the calling thread
 *
 * Decodes the Linux RISC-V wait-status argument.
 * Context: User syscall context; never returns to the exiting thread.
 */
uint64 sys_linux_exit(void)
{
	int status;

	argint(0, &status);
	process_thread_exit(status, 0);
	return 0;
}

/**
 * sys_linux_set_tid_address() - Record clear-tid storage for thread exit
 *
 * Decodes the Linux RISC-V user address argument.
 * Context: User syscall context; updates only the current thread.
 * Return: Current thread ID.
 */
uint64 sys_linux_set_tid_address(void)
{
	uint64 address;

	argaddr(0, &address);
	cur_thread()->clear_child_tid = address;
	return cur_thread()->tid;
}

/**
 * sys_linux_prctl() - Handle the supported Linux process-control operations
 *
 * Decodes Linux RISC-V register arguments; unsupported operations fail.
 * Context: User syscall context; operation-specific locks and user copies
 * apply.
 * Return: Operation result or a negative Linux errno.
 */
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

/**
 * sys_linux_getpid() - Return the caller's thread-group ID
 *
 * Context: User syscall context; does not sleep.
 * Return: Positive process ID.
 */
uint64 sys_linux_getpid(void)
{
	return cur_proc()->pid;
}

/**
 * sys_linux_getppid() - Return the caller's current parent ID
 *
 * Context: User syscall context; serializes with the process list.
 * Return: Parent process ID, or zero for init.
 */
uint64 sys_linux_getppid(void)
{
	process_t parent = cur_proc()->parent;

	return parent ? parent->pid : 0;
}

/**
 * sys_linux_getuid() - Return the caller's real user ID
 *
 * Context: User syscall context; acquires the process lock.
 * Return: Real user ID.
 */
uint64 sys_linux_getuid(void)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	return credentials.uid;
}

/**
 * sys_linux_geteuid() - Return the caller's effective user ID
 *
 * Context: User syscall context; acquires the process lock.
 * Return: Effective user ID.
 */
uint64 sys_linux_geteuid(void)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	return credentials.euid;
}

/**
 * sys_linux_getgid() - Return the caller's real group ID
 *
 * Context: User syscall context; acquires the process lock.
 * Return: Real group ID.
 */
uint64 sys_linux_getgid(void)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	return credentials.gid;
}

/**
 * sys_linux_getegid() - Return the caller's effective group ID
 *
 * Context: User syscall context; acquires the process lock.
 * Return: Effective group ID.
 */
uint64 sys_linux_getegid(void)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	return credentials.egid;
}

#define LINUX_ID_NO_CHANGE ((uint32)-1)

/* Check whether a requested ID is already one of the caller's saved IDs. */
static int credentials_id_allowed(uint32 requested, uint32 first,
				  uint32 second, uint32 third)
{
	return requested == LINUX_ID_NO_CHANGE || requested == first ||
	       requested == second || requested == third;
}

/**
 * sys_linux_setuid() - Apply Linux real and effective user-ID rules
 *
 * Decodes the Linux RISC-V requested UID argument.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission error.
 */
uint64 sys_linux_setuid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 uid;
	int value, result = 0;

	argint(0, &value);
	uid = value;
	if (uid == LINUX_ID_NO_CHANGE)
		return -LINUX_EINVAL;
	spinlock_acquire(&process->lock);
	if (!credentials->euid) {
		credentials->uid = uid;
		credentials->euid = uid;
		credentials->suid = uid;
		credentials->fsuid = uid;
	} else if (uid == credentials->uid || uid == credentials->suid) {
		credentials->euid = uid;
		credentials->fsuid = uid;
	} else {
		result = -LINUX_EPERM;
	}
	spinlock_release(&process->lock);
	return result;
}

/**
 * sys_linux_setgid() - Apply Linux real and effective group-ID rules
 *
 * Decodes the Linux RISC-V requested GID argument.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission error.
 */
uint64 sys_linux_setgid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 gid;
	int value, result = 0;

	argint(0, &value);
	gid = value;
	if (gid == LINUX_ID_NO_CHANGE)
		return -LINUX_EINVAL;
	spinlock_acquire(&process->lock);
	if (!credentials->euid) {
		credentials->gid = gid;
		credentials->egid = gid;
		credentials->sgid = gid;
		credentials->fsgid = gid;
	} else if (gid == credentials->gid || gid == credentials->sgid) {
		credentials->egid = gid;
		credentials->fsgid = gid;
	} else {
		result = -LINUX_EPERM;
	}
	spinlock_release(&process->lock);
	return result;
}

/**
 * sys_linux_setreuid() - Update real and effective user IDs
 *
 * Decodes Linux RISC-V UID arguments; -1 preserves an existing value.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission/argument error.
 */
uint64 sys_linux_setreuid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 ruid, euid, old_uid;
	int real, effective, result = 0;

	argint(0, &real);
	argint(1, &effective);
	ruid = real;
	euid = effective;
	spinlock_acquire(&process->lock);
	old_uid = credentials->uid;
	if (credentials->euid &&
	    (!credentials_id_allowed(ruid, credentials->uid,
				     credentials->euid,
				     credentials->uid) ||
	     !credentials_id_allowed(euid, credentials->uid,
				     credentials->euid,
				     credentials->suid))) {
		result = -LINUX_EPERM;
		goto out;
	}
	if (ruid != LINUX_ID_NO_CHANGE)
		credentials->uid = ruid;
	if (euid != LINUX_ID_NO_CHANGE)
		credentials->euid = euid;
	if (ruid != LINUX_ID_NO_CHANGE ||
	    (euid != LINUX_ID_NO_CHANGE && euid != old_uid))
		credentials->suid = credentials->euid;
	if (euid != LINUX_ID_NO_CHANGE)
		credentials->fsuid = credentials->euid;
out:
	spinlock_release(&process->lock);
	return result;
}

/**
 * sys_linux_setregid() - Update real and effective group IDs
 *
 * Decodes Linux RISC-V GID arguments; -1 preserves an existing value.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission/argument error.
 */
uint64 sys_linux_setregid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 rgid, egid, old_gid;
	int real, effective, result = 0;

	argint(0, &real);
	argint(1, &effective);
	rgid = real;
	egid = effective;
	spinlock_acquire(&process->lock);
	old_gid = credentials->gid;
	if (credentials->euid &&
	    (!credentials_id_allowed(rgid, credentials->gid,
				     credentials->egid,
				     credentials->gid) ||
	     !credentials_id_allowed(egid, credentials->gid,
				     credentials->egid,
				     credentials->sgid))) {
		result = -LINUX_EPERM;
		goto out;
	}
	if (rgid != LINUX_ID_NO_CHANGE)
		credentials->gid = rgid;
	if (egid != LINUX_ID_NO_CHANGE)
		credentials->egid = egid;
	if (rgid != LINUX_ID_NO_CHANGE ||
	    (egid != LINUX_ID_NO_CHANGE && egid != old_gid))
		credentials->sgid = credentials->egid;
	if (egid != LINUX_ID_NO_CHANGE)
		credentials->fsgid = credentials->egid;
out:
	spinlock_release(&process->lock);
	return result;
}

/**
 * sys_linux_setresuid() - Update real, effective, and saved user IDs
 *
 * Decodes Linux RISC-V UID arguments; -1 preserves an existing value.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission/argument error.
 */
uint64 sys_linux_setresuid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 ruid, euid, suid;
	int real, effective, saved, result = 0;

	argint(0, &real);
	argint(1, &effective);
	argint(2, &saved);
	ruid = real;
	euid = effective;
	suid = saved;
	spinlock_acquire(&process->lock);
	if (credentials->euid &&
	    (!credentials_id_allowed(ruid, credentials->uid,
				     credentials->euid,
				     credentials->suid) ||
	     !credentials_id_allowed(euid, credentials->uid,
				     credentials->euid,
				     credentials->suid) ||
	     !credentials_id_allowed(suid, credentials->uid,
				     credentials->euid,
				     credentials->suid))) {
		result = -LINUX_EPERM;
		goto out;
	}
	if (ruid != LINUX_ID_NO_CHANGE)
		credentials->uid = ruid;
	if (euid != LINUX_ID_NO_CHANGE)
		credentials->euid = euid;
	if (suid != LINUX_ID_NO_CHANGE)
		credentials->suid = suid;
	if (euid != LINUX_ID_NO_CHANGE)
		credentials->fsuid = credentials->euid;
out:
	spinlock_release(&process->lock);
	return result;
}

/**
 * sys_linux_setresgid() - Update real, effective, and saved group IDs
 *
 * Decodes Linux RISC-V GID arguments; -1 preserves an existing value.
 * Context: User syscall context; acquires the process lock.
 * Return: Zero or a negative Linux permission/argument error.
 */
uint64 sys_linux_setresgid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 rgid, egid, sgid;
	int real, effective, saved, result = 0;

	argint(0, &real);
	argint(1, &effective);
	argint(2, &saved);
	rgid = real;
	egid = effective;
	sgid = saved;
	spinlock_acquire(&process->lock);
	if (credentials->euid &&
	    (!credentials_id_allowed(rgid, credentials->gid,
				     credentials->egid,
				     credentials->sgid) ||
	     !credentials_id_allowed(egid, credentials->gid,
				     credentials->egid,
				     credentials->sgid) ||
	     !credentials_id_allowed(sgid, credentials->gid,
				     credentials->egid,
				     credentials->sgid))) {
		result = -LINUX_EPERM;
		goto out;
	}
	if (rgid != LINUX_ID_NO_CHANGE)
		credentials->gid = rgid;
	if (egid != LINUX_ID_NO_CHANGE)
		credentials->egid = egid;
	if (sgid != LINUX_ID_NO_CHANGE)
		credentials->sgid = sgid;
	if (egid != LINUX_ID_NO_CHANGE)
		credentials->fsgid = credentials->egid;
out:
	spinlock_release(&process->lock);
	return result;
}

/* Copy the caller's real, effective, and saved IDs to user memory. */
static uint64 credentials_copy_ids(int group)
{
	struct process_credentials credentials;
	process_t process = cur_proc();
	uint64 real_address, effective_address, saved_address;
	uint32 real, effective, saved;

	argaddr(0, &real_address);
	argaddr(1, &effective_address);
	argaddr(2, &saved_address);
	process_credentials_get(&credentials);
	if (group) {
		real = credentials.gid;
		effective = credentials.egid;
		saved = credentials.sgid;
	} else {
		real = credentials.uid;
		effective = credentials.euid;
		saved = credentials.suid;
	}
	if (copyout(process->pagetable, real_address, (char *)&real,
		    sizeof(real)) < 0 ||
	    copyout(process->pagetable, effective_address,
		    (char *)&effective, sizeof(effective)) < 0 ||
	    copyout(process->pagetable, saved_address, (char *)&saved,
		    sizeof(saved)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

/**
 * sys_linux_getresuid() - Copy real, effective, and saved user IDs
 *
 * Decodes three Linux RISC-V user buffer arguments.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_getresuid(void)
{
	return credentials_copy_ids(0);
}

/**
 * sys_linux_getresgid() - Copy real, effective, and saved group IDs
 *
 * Decodes three Linux RISC-V user buffer arguments.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Zero or a negative Linux errno.
 */
uint64 sys_linux_getresgid(void)
{
	return credentials_copy_ids(1);
}

/**
 * sys_linux_setfsuid() - Set filesystem user ID under Linux rules
 *
 * Decodes the Linux RISC-V requested UID argument.
 * Context: User syscall context; acquires the process lock.
 * Return: Previous filesystem user ID.
 */
uint64 sys_linux_setfsuid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 uid, old;
	int value;

	argint(0, &value);
	uid = value;
	spinlock_acquire(&process->lock);
	old = credentials->fsuid;
	if (uid != LINUX_ID_NO_CHANGE &&
	    (!credentials->euid || uid == credentials->uid ||
	     uid == credentials->euid || uid == credentials->suid ||
	     uid == credentials->fsuid))
		credentials->fsuid = uid;
	spinlock_release(&process->lock);
	return old;
}

/**
 * sys_linux_setfsgid() - Set filesystem group ID under Linux rules
 *
 * Decodes the Linux RISC-V requested GID argument.
 * Context: User syscall context; acquires the process lock.
 * Return: Previous filesystem group ID.
 */
uint64 sys_linux_setfsgid(void)
{
	process_t process = cur_proc();
	struct process_credentials *credentials = &process->credentials;
	uint32 gid, old;
	int value;

	argint(0, &value);
	gid = value;
	spinlock_acquire(&process->lock);
	old = credentials->fsgid;
	if (gid != LINUX_ID_NO_CHANGE &&
	    (!credentials->euid || gid == credentials->gid ||
	     gid == credentials->egid || gid == credentials->sgid ||
	     gid == credentials->fsgid))
		credentials->fsgid = gid;
	spinlock_release(&process->lock);
	return old;
}

/**
 * sys_linux_getgroups() - Report supplementary groups
 *
 * Decodes Linux RISC-V count and optional user-buffer arguments.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Group count or a negative Linux errno.
 */
uint64 sys_linux_getgroups(void)
{
	struct process_credentials credentials;
	process_t process = cur_proc();
	uint64 address;
	int size;

	argint(0, &size);
	argaddr(1, &address);
	if (size < 0)
		return -LINUX_EINVAL;
	process_credentials_get(&credentials);
	if (!size)
		return credentials.group_count;
	if ((uint32)size < credentials.group_count)
		return -LINUX_EINVAL;
	if (credentials.group_count &&
	    copyout(process->pagetable, address, (char *)credentials.groups,
		    credentials.group_count * sizeof(credentials.groups[0])) < 0)
		return -LINUX_EFAULT;
	return credentials.group_count;
}

/**
 * sys_linux_setgroups() - Replace supplementary groups
 *
 * Decodes Linux RISC-V count and user-buffer arguments.
 * Context: User syscall context; acquires the process lock and may fault.
 * Return: Zero or a negative Linux permission/argument/fault error.
 */
uint64 sys_linux_setgroups(void)
{
	process_t process = cur_proc();
	uint32 groups[PROCESS_GROUP_MAX];
	uint64 address;
	int size;

	argint(0, &size);
	argaddr(1, &address);
	if (size < 0 || size > PROCESS_GROUP_MAX)
		return -LINUX_EINVAL;
	if (size && copyin(process->pagetable, (char *)groups, address,
			   size * sizeof(groups[0])) < 0)
		return -LINUX_EFAULT;
	spinlock_acquire(&process->lock);
	if (process->credentials.euid) {
		spinlock_release(&process->lock);
		return -LINUX_EPERM;
	}
	process->credentials.group_count = size;
	if (size)
		memmove(process->credentials.groups, groups,
			size * sizeof(groups[0]));
	if (size < PROCESS_GROUP_MAX)
		memset(process->credentials.groups + size, 0,
		       (PROCESS_GROUP_MAX - size) * sizeof(groups[0]));
	spinlock_release(&process->lock);
	return 0;
}

/**
 * sys_linux_gettid() - Return the calling thread ID
 *
 * Context: User syscall context; does not sleep.
 * Return: Positive thread ID.
 */
uint64 sys_linux_gettid(void)
{
	return cur_thread()->tid;
}

/**
 * sys_linux_setpgid() - Assign a process to a process group
 *
 * Decodes Linux RISC-V PID and PGID arguments.
 * Context: User syscall context; takes process-list locks.
 * Return: Zero or a negative Linux error.
 */
uint64 sys_linux_setpgid(void)
{
	int pgid, pid;

	argint(0, &pid);
	argint(1, &pgid);
	return process_setpgid(pid, pgid);
}

/**
 * sys_linux_getpgid() - Return a process group identifier
 *
 * Decodes the Linux RISC-V PID argument.
 * Context: User syscall context; takes the process-list lock.
 * Return: Process group ID or a negative Linux error.
 */
uint64 sys_linux_getpgid(void)
{
	int pid;

	argint(0, &pid);
	return process_getpgid(pid);
}

/**
 * sys_linux_getsid() - Return a process session identifier
 *
 * Decodes the Linux RISC-V PID argument.
 * Context: User syscall context; takes the process-list lock.
 * Return: Session ID or a negative Linux error.
 */
uint64 sys_linux_getsid(void)
{
	int pid;

	argint(0, &pid);
	return process_getsid(pid);
}

/**
 * sys_linux_setsid() - Create a session and process group
 *
 * Context: User syscall context; takes the process-list lock.
 * Return: New session ID or a negative Linux permission error.
 */
uint64 sys_linux_setsid(void)
{
	return process_setsid();
}

/**
 * sys_linux_getrandom() - Fill a user buffer from the kernel random source
 *
 * Decodes Linux RISC-V buffer, length, and flags arguments.
 * Context: User syscall context; may fault on the user copy.
 * Return: Bytes copied or a negative Linux errno.
 */
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

/**
 * sys_linux_setpriority() - Translate Linux priority to scheduler niceness
 *
 * Decodes Linux RISC-V selector, target ID, and priority arguments.
 * Context: User syscall context; serializes with scheduler state.
 * Return: Zero or a negative Linux error.
 */
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

/**
 * sys_linux_getpriority() - Translate scheduler niceness to Linux priority
 *
 * Decodes Linux RISC-V selector and target ID arguments.
 * Context: User syscall context; serializes with scheduler state.
 * Return: Linux priority or a negative Linux error.
 */
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

/**
 * sys_linux_umask() - Replace the calling process creation mask
 *
 * Decodes the Linux RISC-V mask argument.
 * Context: User syscall context; acquires the process lock.
 * Return: Previous mask.
 */
uint64 sys_linux_umask(void)
{
	int mask;

	argint(0, &mask);
	return process_umask_set(mask);
}

/**
 * sys_linux_riscv_flush_icache() - Synchronize RISC-V instruction caches
 *
 * Decodes Linux RISC-V address, length, and flags arguments.
 * Context: User syscall context; may send cross-hart IPIs.
 * Return: Zero or a negative Linux argument error.
 */
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

/**
 * sys_linux_clone() - Create a process or thread from Linux clone arguments
 *
 * Decodes Linux RISC-V flags, stack, TID, TLS, and child-TID arguments.
 * Context: User syscall context; may allocate and sleep.
 * Return: Child PID/TID or a negative Linux error.
 */
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
	if (flags == (LINUX_CLONE_VM | LINUX_CLONE_VFORK |
	              LINUX_SIGCHLD))
		return process_vfork(child_stack);
	if (flags != LINUX_SIGCHLD)
		return -LINUX_EINVAL;
	pid = process_fork(child_stack);
	return pid < 0 ? -LINUX_ENOMEM : pid;
}

/**
 * sys_linux_wait4() - Observe or reap a child state change
 *
 * Decodes Linux RISC-V selector, status, options, and rusage arguments.
 * Context: User syscall context; may sleep interruptibly.
 * Return: Child PID, zero for WNOHANG, or a negative Linux error.
 */
uint64 sys_linux_wait4(void)
{
	uint64 status_address, usage_address;
	int options, result, target;

	argint(0, &target);
	argaddr(1, &status_address);
	argint(2, &options);
	argaddr(3, &usage_address);
	if (options & ~(LINUX_WNOHANG | LINUX_WUNTRACED |
	                LINUX_WCONTINUED))
		return -LINUX_EINVAL;
	result = process_wait(target, status_address, usage_address, options);
	if (result == PROCESS_WAIT_FAULT)
		return -LINUX_EFAULT;
	if (result == PROCESS_WAIT_INTR)
		return -SIGNAL_RESTART_SYS;
	if (result < 0)
		return -LINUX_ECHILD;
	return result;
}
