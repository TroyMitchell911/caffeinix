#ifndef __CAFFEINIX_KERNEL_LINUX_UAPI_H
#define __CAFFEINIX_KERNEL_LINUX_UAPI_H

#include <typedefs.h>

/* Linux RISC-V uses the asm-generic syscall number space. */
#define LINUX_SYS_ioctl             29
#define LINUX_SYS_writev            66
#define LINUX_SYS_exit_group        94
#define LINUX_SYS_set_tid_address   96
#define LINUX_SYS_execve           221

#define LINUX_EBADF                  9
#define LINUX_EFAULT                14
#define LINUX_EINVAL                22
#define LINUX_ENOTTY                25
#define LINUX_ENOSYS                38

#define LINUX_IOV_MAX               16

struct linux_iovec {
	uint64 base;
	uint64 len;
};

/* ELF auxiliary vector tags used by static musl startup. */
#define LINUX_AT_NULL                0
#define LINUX_AT_PHDR                3
#define LINUX_AT_PHENT               4
#define LINUX_AT_PHNUM               5
#define LINUX_AT_PAGESZ              6
#define LINUX_AT_ENTRY               9
#define LINUX_AT_UID                11
#define LINUX_AT_EUID               12
#define LINUX_AT_GID                13
#define LINUX_AT_EGID               14
#define LINUX_AT_SECURE             23
#define LINUX_AT_RANDOM             25
#define LINUX_AT_EXECFN             31

#endif
