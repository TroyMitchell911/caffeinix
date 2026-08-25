#include <cpu.h>
#include <linux_uapi.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>

uint64 sys_linux_membarrier(void)
{
	process_t process = cur_proc();
	int command, flags;

	argint(0, &command);
	argint(1, &flags);
	if (flags)
		return -LINUX_EINVAL;
	switch (command) {
	case LINUX_MEMBARRIER_CMD_QUERY:
		return LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED |
		       LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
	case LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
		__atomic_store_n(&process->membarrier_private_expedited, 1,
		                 __ATOMIC_RELEASE);
		return 0;
	case LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		if (!__atomic_load_n(&process->membarrier_private_expedited,
		                     __ATOMIC_ACQUIRE))
			return -LINUX_EPERM;
		cpu_membarrier();
		return 0;
	default:
		return -LINUX_EINVAL;
	}
}
