#include <debug.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <printk.h>
#include <scheduler.h>
#include <signal.h>
#include <syscall.h>
#include <vfs.h>
#include <vm.h>

int64 linux_error(int result)
{
	switch (result) {
	case VFS_ERR_PERM:
		return -LINUX_EPERM;
	case VFS_ERR_NOENT:
		return -LINUX_ENOENT;
	case VFS_ERR_BADF:
		return -LINUX_EBADF;
	case VFS_ERR_EXIST:
		return -LINUX_EEXIST;
	case VFS_ERR_NOTDIR:
		return -LINUX_ENOTDIR;
	case VFS_ERR_ISDIR:
		return -LINUX_EISDIR;
	case VFS_ERR_INVAL:
		return -LINUX_EINVAL;
	case VFS_ERR_MFILE:
		return -LINUX_EMFILE;
	case VFS_ERR_NOSPC:
		return -LINUX_ENOSPC;
	case VFS_ERR_NOTEMPTY:
		return -LINUX_ENOTEMPTY;
	case VFS_ERR_NODEV:
		return -LINUX_ENODEV;
	case VFS_ERR_NOMEM:
		return -LINUX_ENOMEM;
	case VFS_ERR_NOTSUPP:
		return -LINUX_EOPNOTSUPP;
	case VFS_ERR_NAMETOOLONG:
		return -LINUX_ENAMETOOLONG;
	case VFS_ERR_BUSY:
		return -LINUX_EBUSY;
	case VFS_ERR_LOOP:
		return -LINUX_ELOOP;
	case VFS_ERR_XDEV:
		return -LINUX_EXDEV;
	case VFS_ERR_MLINK:
		return -LINUX_EMLINK;
	case VFS_ERR_NOTTY:
		return -LINUX_ENOTTY;
	case VFS_ERR_NXIO:
		return -LINUX_ENXIO;
	case VFS_ERR_FAULT:
		return -LINUX_EFAULT;
	case VFS_ERR_IO:
		return -LINUX_EIO;
	case VFS_ERR_SPIPE:
		return -LINUX_ESPIPE;
	case VFS_ERR_AGAIN:
		return -LINUX_EAGAIN;
	case VFS_ERR_NOTSOCK:
		return -LINUX_ENOTSOCK;
	case VFS_ERR_DESTADDRREQ:
		return -LINUX_EDESTADDRREQ;
	case VFS_ERR_MSGSIZE:
		return -LINUX_EMSGSIZE;
	case VFS_ERR_PROTOTYPE:
		return -LINUX_EPROTOTYPE;
	case VFS_ERR_NOPROTOOPT:
		return -LINUX_ENOPROTOOPT;
	case VFS_ERR_PROTONOSUPPORT:
		return -LINUX_EPROTONOSUPPORT;
	case VFS_ERR_SOCKTNOSUPPORT:
		return -LINUX_ESOCKTNOSUPPORT;
	case VFS_ERR_AFNOSUPPORT:
		return -LINUX_EAFNOSUPPORT;
	case VFS_ERR_ADDRINUSE:
		return -LINUX_EADDRINUSE;
	case VFS_ERR_ADDRNOTAVAIL:
		return -LINUX_EADDRNOTAVAIL;
	case VFS_ERR_NETDOWN:
		return -LINUX_ENETDOWN;
	case VFS_ERR_NETUNREACH:
		return -LINUX_ENETUNREACH;
	case VFS_ERR_CONNABORTED:
		return -LINUX_ECONNABORTED;
	case VFS_ERR_CONNRESET:
		return -LINUX_ECONNRESET;
	case VFS_ERR_NOBUFS:
		return -LINUX_ENOBUFS;
	case VFS_ERR_ISCONN:
		return -LINUX_EISCONN;
	case VFS_ERR_NOTCONN:
		return -LINUX_ENOTCONN;
	case VFS_ERR_SHUTDOWN:
		return -LINUX_ESHUTDOWN;
	case VFS_ERR_TIMEDOUT:
		return -LINUX_ETIMEDOUT;
	case VFS_ERR_CONNREFUSED:
		return -LINUX_ECONNREFUSED;
	case VFS_ERR_HOSTUNREACH:
		return -LINUX_EHOSTUNREACH;
	case VFS_ERR_ALREADY:
		return -LINUX_EALREADY;
	case VFS_ERR_INPROGRESS:
		return -LINUX_EINPROGRESS;
	case VFS_ERR_PIPE:
		return -LINUX_EPIPE;
	case VFS_ERR_INTR:
		return -LINUX_EINTR;
	default:
		return -LINUX_EIO;
	}
}

int copy_user_iov(uint64 address, int count, struct vfs_iovec **result,
		  unsigned int *order)
{
	struct linux_iovec linux_iov;
	struct vfs_iovec *iovecs;
	uint64 bytes, capacity = PGSIZE, total = 0;
	int i;

	*result = 0;
	*order = 0;
	if (count < 0 || count > LINUX_IOV_MAX)
		return -LINUX_EINVAL;
	if (!count)
		return 0;
	bytes = count * sizeof(*iovecs);
	while (capacity < bytes) {
		capacity <<= 1;
		(*order)++;
	}
	iovecs = alloc_pages(*order, 0);
	if (!iovecs)
		return -LINUX_ENOMEM;

	for (i = 0; i < count; i++) {
		if (copyin(cur_proc()->pagetable, (char *)&linux_iov,
		           address + i * sizeof(linux_iov),
		           sizeof(linux_iov)) < 0) {
			free_pages(iovecs, *order);
			return -LINUX_EFAULT;
		}
		if (linux_iov.len > 0x7fffffff - total) {
			free_pages(iovecs, *order);
			return -LINUX_EINVAL;
		}
		iovecs[i].base = linux_iov.base;
		iovecs[i].length = linux_iov.len;
		total += linux_iov.len;
	}
	*result = iovecs;
	return 0;
}

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
extern uint64 sys_linux_futex(void);
extern uint64 sys_linux_set_robust_list(void);
extern uint64 sys_linux_get_robust_list(void);
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
extern uint64 sys_linux_msync(void);
extern uint64 sys_linux_membarrier(void);
extern uint64 sys_linux_munmap(void);
extern uint64 sys_linux_newfstatat(void);
extern uint64 sys_linux_openat(void);
extern uint64 sys_linux_pipe2(void);
extern uint64 sys_linux_prctl(void);
extern uint64 sys_linux_pread64(void);
extern uint64 sys_linux_preadv(void);
extern uint64 sys_linux_preadv2(void);
extern uint64 sys_linux_pwrite64(void);
extern uint64 sys_linux_pwritev(void);
extern uint64 sys_linux_pwritev2(void);
extern uint64 sys_linux_read(void);
extern uint64 sys_linux_readv(void);
extern uint64 sys_linux_readlinkat(void);
extern uint64 sys_linux_renameat2(void);
extern uint64 sys_linux_sendfile(void);
extern uint64 sys_linux_set_tid_address(void);
extern uint64 sys_linux_clone(void);
extern uint64 sys_linux_clock_gettime(void);
extern uint64 sys_linux_kill(void);
extern uint64 sys_linux_tkill(void);
extern uint64 sys_linux_tgkill(void);
extern uint64 sys_linux_sigaltstack(void);
extern uint64 sys_linux_rt_sigsuspend(void);
extern uint64 sys_linux_rt_sigaction(void);
extern uint64 sys_linux_rt_sigprocmask(void);
extern uint64 sys_linux_rt_sigpending(void);
extern uint64 sys_linux_rt_sigtimedwait(void);
extern uint64 sys_linux_rt_sigreturn(void);
extern uint64 sys_linux_setpriority(void);
extern uint64 sys_linux_getpriority(void);
extern uint64 sys_linux_setpgid(void);
extern uint64 sys_linux_getpgid(void);
extern uint64 sys_linux_getsid(void);
extern uint64 sys_linux_setsid(void);
extern uint64 sys_linux_getrandom(void);
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
extern uint64 sys_linux_riscv_flush_icache(void);
extern uint64 sys_linux_ftruncate(void);

typedef uint64 (*syscall_t)(void);

static syscall_t linux_syscalls[LINUX_SYS_pwritev2 + 1] = {
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
	[LINUX_SYS_pipe2] = sys_linux_pipe2,
	[LINUX_SYS_getdents64] = sys_linux_getdents64,
	[LINUX_SYS_lseek] = sys_linux_lseek,
	[LINUX_SYS_read] = sys_linux_read,
	[LINUX_SYS_write] = sys_linux_write,
	[LINUX_SYS_readv] = sys_linux_readv,
	[LINUX_SYS_writev] = sys_linux_writev,
	[LINUX_SYS_pread64] = sys_linux_pread64,
	[LINUX_SYS_pwrite64] = sys_linux_pwrite64,
	[LINUX_SYS_preadv] = sys_linux_preadv,
	[LINUX_SYS_pwritev] = sys_linux_pwritev,
	[LINUX_SYS_sendfile] = sys_linux_sendfile,
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
	[LINUX_SYS_futex] = sys_linux_futex,
	[LINUX_SYS_set_robust_list] = sys_linux_set_robust_list,
	[LINUX_SYS_get_robust_list] = sys_linux_get_robust_list,
	[LINUX_SYS_set_tid_address] = sys_linux_set_tid_address,
	[LINUX_SYS_clock_gettime] = sys_linux_clock_gettime,
	[LINUX_SYS_kill] = sys_linux_kill,
	[LINUX_SYS_tkill] = sys_linux_tkill,
	[LINUX_SYS_tgkill] = sys_linux_tgkill,
	[LINUX_SYS_sigaltstack] = sys_linux_sigaltstack,
	[LINUX_SYS_rt_sigsuspend] = sys_linux_rt_sigsuspend,
	[LINUX_SYS_rt_sigaction] = sys_linux_rt_sigaction,
	[LINUX_SYS_rt_sigprocmask] = sys_linux_rt_sigprocmask,
	[LINUX_SYS_rt_sigpending] = sys_linux_rt_sigpending,
	[LINUX_SYS_rt_sigtimedwait] = sys_linux_rt_sigtimedwait,
	[LINUX_SYS_rt_sigreturn] = sys_linux_rt_sigreturn,
	[LINUX_SYS_setpriority] = sys_linux_setpriority,
	[LINUX_SYS_getpriority] = sys_linux_getpriority,
	[LINUX_SYS_setpgid] = sys_linux_setpgid,
	[LINUX_SYS_getpgid] = sys_linux_getpgid,
	[LINUX_SYS_getsid] = sys_linux_getsid,
	[LINUX_SYS_setsid] = sys_linux_setsid,
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
	[LINUX_SYS_msync] = sys_linux_msync,
	[LINUX_SYS_accept4] = sys_linux_accept4,
	[LINUX_SYS_riscv_flush_icache] = sys_linux_riscv_flush_icache,
	[LINUX_SYS_wait4] = sys_linux_wait4,
	[LINUX_SYS_renameat2] = sys_linux_renameat2,
	[LINUX_SYS_getrandom] = sys_linux_getrandom,
	[LINUX_SYS_membarrier] = sys_linux_membarrier,
	[LINUX_SYS_preadv2] = sys_linux_preadv2,
	[LINUX_SYS_pwritev2] = sys_linux_pwritev2,
};

void syscall(void)
{
	uint64 result;
	uint64 nr;
	process_t p = cur_proc();
	thread_t current = cur_thread();

	nr = current->trapframe->a7;
	current->syscall_restart = 0;
	if (nr < NELEM(linux_syscalls) && linux_syscalls[nr]) {
		result = linux_syscalls[nr]();
		if (nr != LINUX_SYS_rt_sigreturn &&
		    (int64)result == -SIGNAL_RESTART_SYS)
			current->syscall_restart = 1;
		current->trapframe->a0 = result;
		return;
	}

	pr_warn("syscall: unsupported Linux call %lu from pid %d (%s)",
		nr, p->pid, p->name);
	current->trapframe->a0 = -LINUX_ENOSYS;
}
