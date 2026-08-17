#include <debug.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <printk.h>
#include <scheduler.h>
#include <syscall.h>
#include <vm.h>

static uint64 argraw(int n)
{
	uint64 *args = &cur_thread()->trapframe->a0;

	if (n >= 0 && n <= 5)
		return args[n];

	PANIC("argraw");
	return 0;
}

int fetch_str_from_user(uint64 user_addr, char *buf, int max)
{
	process_t p = cur_proc();

	if (copyinstr(p->pagetable, buf, user_addr, max) < 0)
		return -1;
	return strlen(buf);
}

int fetch_addr_from_user(uint64 user_addr, uint64 *dst)
{
        process_t p = cur_proc();

	if (copyin(p->pagetable, (char *)dst, user_addr, sizeof(uint64)) != 0)
		return -1;
	return 0;
}

void argint(int n, int *ip)
{
	*ip = argraw(n);
}

void argaddr(int n, uint64 *ap)
{
	*ap = argraw(n);
}

int argstr(int n, char *buf, int max)
{
	uint64 addr;

	argaddr(n, &addr);
	return fetch_str_from_user(addr, buf, max);
}

extern uint64 sys_linux_brk(void);
extern uint64 sys_linux_chdir(void);
extern uint64 sys_linux_close(void);
extern uint64 sys_linux_dup(void);
extern uint64 sys_linux_dup3(void);
extern uint64 sys_linux_execve(void);
extern uint64 sys_linux_exit(void);
extern uint64 sys_linux_exit_group(void);
extern uint64 sys_linux_faccessat(void);
extern uint64 sys_linux_fcntl(void);
extern uint64 sys_linux_fstat(void);
extern uint64 sys_linux_getcwd(void);
extern uint64 sys_linux_getdents64(void);
extern uint64 sys_linux_getegid(void);
extern uint64 sys_linux_geteuid(void);
extern uint64 sys_linux_getgid(void);
extern uint64 sys_linux_getpid(void);
extern uint64 sys_linux_getppid(void);
extern uint64 sys_linux_gettid(void);
extern uint64 sys_linux_getuid(void);
extern uint64 sys_linux_fdatasync(void);
extern uint64 sys_linux_fsync(void);
extern uint64 sys_linux_ioctl(void);
extern uint64 sys_linux_linkat(void);
extern uint64 sys_linux_lseek(void);
extern uint64 sys_linux_mkdirat(void);
extern uint64 sys_linux_mmap(void);
extern uint64 sys_linux_mprotect(void);
extern uint64 sys_linux_munmap(void);
extern uint64 sys_linux_newfstatat(void);
extern uint64 sys_linux_openat(void);
extern uint64 sys_linux_prctl(void);
extern uint64 sys_linux_read(void);
extern uint64 sys_linux_readlinkat(void);
extern uint64 sys_linux_renameat2(void);
extern uint64 sys_linux_set_tid_address(void);
extern uint64 sys_linux_clone(void);
extern uint64 sys_linux_clock_gettime(void);
extern uint64 sys_linux_rt_sigaction(void);
extern uint64 sys_linux_rt_sigprocmask(void);
extern uint64 sys_linux_setpriority(void);
extern uint64 sys_linux_getpriority(void);
extern uint64 sys_linux_symlinkat(void);
extern uint64 sys_linux_sync(void);
extern uint64 sys_linux_umask(void);
extern uint64 sys_linux_unlinkat(void);
extern uint64 sys_linux_utimensat(void);
extern uint64 sys_linux_write(void);
extern uint64 sys_linux_writev(void);
extern uint64 sys_linux_wait4(void);
extern uint64 sys_linux_ppoll(void);
extern uint64 sys_linux_socket(void);
extern uint64 sys_linux_socketpair(void);
extern uint64 sys_linux_bind(void);
extern uint64 sys_linux_listen(void);
extern uint64 sys_linux_accept(void);
extern uint64 sys_linux_connect(void);
extern uint64 sys_linux_getsockname(void);
extern uint64 sys_linux_getpeername(void);
extern uint64 sys_linux_sendto(void);
extern uint64 sys_linux_recvfrom(void);
extern uint64 sys_linux_setsockopt(void);
extern uint64 sys_linux_getsockopt(void);
extern uint64 sys_linux_shutdown(void);
extern uint64 sys_linux_sendmsg(void);
extern uint64 sys_linux_recvmsg(void);
extern uint64 sys_linux_accept4(void);
extern uint64 sys_linux_ftruncate(void);

typedef uint64 (*syscall_t)(void);

static syscall_t linux_syscalls[LINUX_SYS_renameat2 + 1] = {
	[LINUX_SYS_getcwd] = sys_linux_getcwd,
	[LINUX_SYS_dup] = sys_linux_dup,
	[LINUX_SYS_dup3] = sys_linux_dup3,
	[LINUX_SYS_fcntl] = sys_linux_fcntl,
	[LINUX_SYS_ioctl] = sys_linux_ioctl,
	[LINUX_SYS_mkdirat] = sys_linux_mkdirat,
	[LINUX_SYS_unlinkat] = sys_linux_unlinkat,
	[LINUX_SYS_symlinkat] = sys_linux_symlinkat,
	[LINUX_SYS_linkat] = sys_linux_linkat,
	[LINUX_SYS_ftruncate] = sys_linux_ftruncate,
	[LINUX_SYS_faccessat] = sys_linux_faccessat,
	[LINUX_SYS_chdir] = sys_linux_chdir,
	[LINUX_SYS_openat] = sys_linux_openat,
	[LINUX_SYS_close] = sys_linux_close,
	[LINUX_SYS_getdents64] = sys_linux_getdents64,
	[LINUX_SYS_lseek] = sys_linux_lseek,
	[LINUX_SYS_read] = sys_linux_read,
	[LINUX_SYS_write] = sys_linux_write,
	[LINUX_SYS_writev] = sys_linux_writev,
	[LINUX_SYS_ppoll] = sys_linux_ppoll,
	[LINUX_SYS_readlinkat] = sys_linux_readlinkat,
	[LINUX_SYS_newfstatat] = sys_linux_newfstatat,
	[LINUX_SYS_fstat] = sys_linux_fstat,
	[LINUX_SYS_sync] = sys_linux_sync,
	[LINUX_SYS_fsync] = sys_linux_fsync,
	[LINUX_SYS_fdatasync] = sys_linux_fdatasync,
	[LINUX_SYS_utimensat] = sys_linux_utimensat,
	[LINUX_SYS_exit] = sys_linux_exit,
	[LINUX_SYS_exit_group] = sys_linux_exit_group,
	[LINUX_SYS_set_tid_address] = sys_linux_set_tid_address,
	[LINUX_SYS_clock_gettime] = sys_linux_clock_gettime,
	[LINUX_SYS_rt_sigaction] = sys_linux_rt_sigaction,
	[LINUX_SYS_rt_sigprocmask] = sys_linux_rt_sigprocmask,
	[LINUX_SYS_setpriority] = sys_linux_setpriority,
	[LINUX_SYS_getpriority] = sys_linux_getpriority,
	[LINUX_SYS_umask] = sys_linux_umask,
	[LINUX_SYS_prctl] = sys_linux_prctl,
	[LINUX_SYS_getpid] = sys_linux_getpid,
	[LINUX_SYS_getppid] = sys_linux_getppid,
	[LINUX_SYS_getuid] = sys_linux_getuid,
	[LINUX_SYS_geteuid] = sys_linux_geteuid,
	[LINUX_SYS_getgid] = sys_linux_getgid,
	[LINUX_SYS_getegid] = sys_linux_getegid,
	[LINUX_SYS_gettid] = sys_linux_gettid,
	[LINUX_SYS_socket] = sys_linux_socket,
	[LINUX_SYS_socketpair] = sys_linux_socketpair,
	[LINUX_SYS_bind] = sys_linux_bind,
	[LINUX_SYS_listen] = sys_linux_listen,
	[LINUX_SYS_accept] = sys_linux_accept,
	[LINUX_SYS_connect] = sys_linux_connect,
	[LINUX_SYS_getsockname] = sys_linux_getsockname,
	[LINUX_SYS_getpeername] = sys_linux_getpeername,
	[LINUX_SYS_sendto] = sys_linux_sendto,
	[LINUX_SYS_recvfrom] = sys_linux_recvfrom,
	[LINUX_SYS_setsockopt] = sys_linux_setsockopt,
	[LINUX_SYS_getsockopt] = sys_linux_getsockopt,
	[LINUX_SYS_shutdown] = sys_linux_shutdown,
	[LINUX_SYS_sendmsg] = sys_linux_sendmsg,
	[LINUX_SYS_recvmsg] = sys_linux_recvmsg,
	[LINUX_SYS_brk] = sys_linux_brk,
	[LINUX_SYS_munmap] = sys_linux_munmap,
	[LINUX_SYS_clone] = sys_linux_clone,
	[LINUX_SYS_execve] = sys_linux_execve,
	[LINUX_SYS_mmap] = sys_linux_mmap,
	[LINUX_SYS_mprotect] = sys_linux_mprotect,
	[LINUX_SYS_accept4] = sys_linux_accept4,
	[LINUX_SYS_wait4] = sys_linux_wait4,
	[LINUX_SYS_renameat2] = sys_linux_renameat2,
};

void syscall(void)
{
	uint64 nr;
	process_t p = cur_proc();
	thread_t current = cur_thread();

	nr = current->trapframe->a7;
	if (nr < NELEM(linux_syscalls) && linux_syscalls[nr]) {
		current->trapframe->a0 = linux_syscalls[nr]();
		return;
	}

	pr_warn("syscall: unsupported Linux call %lu from pid %d (%s)",
		nr, p->pid, p->name);
	current->trapframe->a0 = -LINUX_ENOSYS;
}
