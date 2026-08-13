#include <linux_uapi.h>
#include <ktime.h>
#include <mem_layout.h>
#include <mystring.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <vm.h>

extern void exit(int cause);

uint64 sys_linux_clock_gettime(void)
{
	struct linux_timespec time;
	process_t process = cur_proc();
	uint64 address, nanoseconds;
	int clock;

	argint(0, &clock);
	argaddr(1, &address);
	if (clock != LINUX_CLOCK_MONOTONIC)
		return -LINUX_EINVAL;
	nanoseconds = ktime_get_ns();
	time.seconds = nanoseconds / 1000000000ULL;
	time.nanoseconds = nanoseconds % 1000000000ULL;
	if (copyout(process->pagetable, address, (char *)&time,
		    sizeof(time)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_exit_group(void)
{
	int status;

	argint(0, &status);
	exit(status);
	return 0;
}

uint64 sys_linux_exit(void)
{
	return sys_linux_exit_group();
}

uint64 sys_linux_set_tid_address(void)
{
	process_t p = cur_proc();
	uint64 address;

	argaddr(0, &address);
	p->clear_child_tid = address;
	return cur_thread()->tid;
}

uint64 sys_linux_brk(void)
{
	process_t p = cur_proc();
	uint64 requested, result;

	argaddr(0, &requested);
	if (!requested)
		return p->brk;
	if (requested < p->brk_start || requested >= p->mmap_top)
		return p->brk;

	if (requested > p->brk) {
		result = vm_alloc(p->pagetable, p->brk, requested, PTE_W);
		if (!result)
			return p->brk;
	} else if (requested < p->brk) {
		vm_dealloc(p->pagetable, p->brk, requested);
	}
	p->brk = requested;
	if (p->sz < requested)
		p->sz = requested;
	return requested;
}

uint64 sys_linux_mmap(void)
{
	process_t p = cur_proc();
	uint64 address, length, offset, start;
	int protection, flags, fd, permissions = 0;

	argaddr(0, &address);
	argaddr(1, &length);
	argint(2, &protection);
	argint(3, &flags);
	argint(4, &fd);
	argaddr(5, &offset);
	(void)fd;
	(void)offset;

	if (!length || length + PGSIZE - 1 < length ||
	    !(flags & LINUX_MAP_ANONYMOUS) ||
	    !(flags & LINUX_MAP_PRIVATE))
		return -LINUX_EINVAL;
	length = PGROUNDUP(length);
	if (protection & LINUX_PROT_WRITE)
		permissions |= PTE_W;
	if (protection & LINUX_PROT_EXEC)
		permissions |= PTE_X;

	if (flags & LINUX_MAP_FIXED) {
		if (address & (PGSIZE - 1))
			return -LINUX_EINVAL;
		if (address + length < address ||
		    address + length > USER_MMAP_TOP)
			return -LINUX_ENOMEM;
		vm_unmap_range(p->pagetable, address, length);
		if (protection == LINUX_PROT_NONE)
			return address;
		if (!vm_alloc(p->pagetable, address, address + length,
		              permissions))
			return -LINUX_ENOMEM;
		return address;
	}

	if (length > p->mmap_top ||
	    p->mmap_top - length <= PGROUNDUP(p->brk))
		return -LINUX_ENOMEM;
	start = PGROUNDDOWN(p->mmap_top - length);
	if (!vm_alloc(p->pagetable, start, start + length, permissions))
		return -LINUX_ENOMEM;
	p->mmap_top = start;
	return start;
}

uint64 sys_linux_munmap(void)
{
	process_t p = cur_proc();
	uint64 address, length;

	argaddr(0, &address);
	argaddr(1, &length);
	if ((address & (PGSIZE - 1)) || !length || address >= MAXVA ||
	    address + length < address || address + length > MAXVA)
		return -LINUX_EINVAL;
	vm_unmap_range(p->pagetable, address, length);
	return 0;
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

uint64 sys_linux_umask(void)
{
	process_t process = cur_proc();
	uint32 old_mask = process->umask;
	int mask;

	argint(0, &mask);
	process->umask = mask & 0777;
	return old_mask;
}

uint64 sys_linux_rt_sigaction(void)
{
	struct linux_sigaction action;
	process_t process = cur_proc();
	uint64 action_address, old_action_address, sigset_size;
	int signal;

	_Static_assert(sizeof(action) ==
	               sizeof(struct process_signal_action),
	               "signal action layout mismatch");
	argint(0, &signal);
	argaddr(1, &action_address);
	argaddr(2, &old_action_address);
	argaddr(3, &sigset_size);
	if (signal < 1 || signal > 64 ||
	    sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (old_action_address &&
	    copyout(process->pagetable, old_action_address,
	            (char *)&process->signal_actions[signal - 1],
	            sizeof(action)) < 0)
		return -LINUX_EFAULT;
	if (!action_address)
		return 0;
	if (signal == LINUX_SIGKILL || signal == LINUX_SIGSTOP)
		return -LINUX_EINVAL;
	if (copyin(process->pagetable, (char *)&action, action_address,
	           sizeof(action)) < 0)
		return -LINUX_EFAULT;
	memmove(&process->signal_actions[signal - 1], &action,
	        sizeof(action));
	return 0;
}

uint64 sys_linux_rt_sigprocmask(void)
{
	process_t process = cur_proc();
	uint64 mask, mask_address, old_mask_address, sigset_size;
	int how;

	argint(0, &how);
	argaddr(1, &mask_address);
	argaddr(2, &old_mask_address);
	argaddr(3, &sigset_size);
	if (sigset_size != LINUX_SIGSET_SIZE)
		return -LINUX_EINVAL;
	if (old_mask_address &&
	    copyout(process->pagetable, old_mask_address,
	            (char *)&process->signal_mask, sizeof(uint64)) < 0)
		return -LINUX_EFAULT;
	if (!mask_address)
		return 0;
	if (copyin(process->pagetable, (char *)&mask, mask_address,
	           sizeof(mask)) < 0)
		return -LINUX_EFAULT;
	mask &= ~(1ULL << (LINUX_SIGKILL - 1));
	mask &= ~(1ULL << (LINUX_SIGSTOP - 1));
	if (how == LINUX_SIG_BLOCK)
		process->signal_mask |= mask;
	else if (how == LINUX_SIG_UNBLOCK)
		process->signal_mask &= ~mask;
	else if (how == LINUX_SIG_SETMASK)
		process->signal_mask = mask;
	else
		return -LINUX_EINVAL;
	return 0;
}

uint64 sys_linux_clone(void)
{
	uint64 child_stack, flags;
	int pid;

	argaddr(0, &flags);
	argaddr(1, &child_stack);
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
	if ((target < -1) || target == 0 || options & ~LINUX_WNOHANG)
		return -LINUX_EINVAL;
	result = process_wait(target, status_address,
	                      options & LINUX_WNOHANG);
	if (result == -2)
		return -LINUX_EFAULT;
	if (result < 0)
		return -LINUX_ECHILD;
	return result;
}
