/*
 * Linux RISC-V system-call argument decoding and error conversion.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#ifndef __CAFFEINIX_KERNEL_SYSCALL_H
#define __CAFFEINIX_KERNEL_SYSCALL_H

#ifndef __ASSEMBLER__
#include <typedefs.h>

struct vfs_iovec;
#endif

#ifndef __ASSEMBLER__
/**
 * fetch_str_from_user() - Copy a NUL-terminated string from current user VM
 * @user_addr: User virtual address of the string.
 * @buf: Kernel destination buffer.
 * @max: Positive capacity of @buf in bytes.
 *
 * Context: System-call context; may fault while copying.
 * Return: String length excluding NUL, or a negative error.
 */
int fetch_str_from_user(uint64 user_addr, char* buf, int max);
/**
 * fetch_addr_from_user() - Read one machine word from current user memory
 * @user_addr: User virtual address of the word.
 * @dst: Kernel output pointer.
 *
 * Context: System-call context; may fault while copying.
 * Return: Zero or a negative copy error.
 */
int fetch_addr_from_user(uint64 user_addr, uint64* dst);
/* Extract integer, address, or string arguments from the current trap frame. */
void argint(int n, int *ip);
void argaddr(int n, uint64 *ap);
int argstr(int n, char *buf, int max);
/**
 * linux_error() - Translate an internal VFS result to a Linux errno return
 * @result: Internal negative VFS result.
 *
 * Return: Negative Linux errno; non-negative values are passed through.
 */
int64 linux_error(int result);
int copy_user_iov(uint64 address, int count, struct vfs_iovec **result,
		  unsigned int *order);
#endif

#endif
