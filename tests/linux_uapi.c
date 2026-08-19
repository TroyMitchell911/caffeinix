/* Linux 6.10 RISC-V UAPI is the recorded compatibility baseline. */
#include <stddef.h>

#include <linux_uapi.h>

#include <asm/ioctls.h>
#include <asm/ptrace.h>
#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/stat.h>
#include <asm/termbits.h>
#include <asm/unistd.h>
#include <asm/ucontext.h>
#include <asm-generic/poll.h>
#include <asm-generic/siginfo.h>
#include <asm-generic/socket.h>
#include <asm-generic/termios.h>
#include <linux/in.h>
#include <linux/futex.h>
#include <linux/mman.h>
#include <linux/membarrier.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/tcp.h>
#include <linux/time_types.h>

_Static_assert(LINUX_SYS_getcwd == __NR_getcwd, "getcwd number");
_Static_assert(LINUX_SYS_openat == __NR_openat, "openat number");
_Static_assert(LINUX_SYS_symlinkat == __NR_symlinkat,
	       "symlinkat number");
_Static_assert(LINUX_SYS_linkat == __NR_linkat, "linkat number");
_Static_assert(LINUX_SYS_ftruncate == __NR_ftruncate,
	       "ftruncate number");
_Static_assert(LINUX_SYS_ppoll == __NR_ppoll, "ppoll number");
_Static_assert(LINUX_SYS_pread64 == __NR_pread64, "pread64 number");
_Static_assert(LINUX_SYS_socket == __NR_socket, "socket number");
_Static_assert(LINUX_SYS_accept4 == __NR_accept4, "accept4 number");
_Static_assert(LINUX_SYS_getdents64 == __NR_getdents64,
	       "getdents64 number");
_Static_assert(LINUX_SYS_readlinkat == __NR_readlinkat,
	       "readlinkat number");
_Static_assert(LINUX_SYS_sync == __NR_sync, "sync number");
_Static_assert(LINUX_SYS_fsync == __NR_fsync, "fsync number");
_Static_assert(LINUX_SYS_umask == __NR_umask, "umask number");
_Static_assert(LINUX_SYS_rt_sigaction == __NR_rt_sigaction,
	       "rt_sigaction number");
_Static_assert(LINUX_SYS_kill == __NR_kill, "kill number");
_Static_assert(LINUX_SYS_tkill == __NR_tkill, "tkill number");
_Static_assert(LINUX_SYS_tgkill == __NR_tgkill, "tgkill number");
_Static_assert(LINUX_SYS_sigaltstack == __NR_sigaltstack,
	       "sigaltstack number");
_Static_assert(LINUX_SYS_rt_sigsuspend == __NR_rt_sigsuspend,
	       "rt_sigsuspend number");
_Static_assert(LINUX_SYS_rt_sigprocmask == __NR_rt_sigprocmask,
	       "rt_sigprocmask number");
_Static_assert(LINUX_SYS_rt_sigpending == __NR_rt_sigpending,
	       "rt_sigpending number");
_Static_assert(LINUX_SYS_rt_sigtimedwait == __NR_rt_sigtimedwait,
	       "rt_sigtimedwait number");
_Static_assert(LINUX_SYS_rt_sigreturn == __NR_rt_sigreturn,
	       "rt_sigreturn number");
_Static_assert(LINUX_SYS_setpriority == __NR_setpriority,
	       "setpriority number");
_Static_assert(LINUX_SYS_getpriority == __NR_getpriority,
	       "getpriority number");
_Static_assert(LINUX_SYS_clock_gettime == __NR_clock_gettime,
	       "clock_gettime number");
_Static_assert(LINUX_SYS_clone == __NR_clone, "clone number");
_Static_assert(LINUX_SYS_futex == __NR_futex, "futex number");
_Static_assert(LINUX_SYS_set_robust_list == __NR_set_robust_list,
	       "set_robust_list number");
_Static_assert(LINUX_SYS_get_robust_list == __NR_get_robust_list,
	       "get_robust_list number");
_Static_assert(LINUX_SYS_membarrier == __NR_membarrier,
	       "membarrier number");
_Static_assert(LINUX_SYS_execve == __NR_execve, "execve number");
_Static_assert(LINUX_SYS_mprotect == __NR_mprotect, "mprotect number");
_Static_assert(LINUX_SYS_msync == __NR_msync, "msync number");
_Static_assert(LINUX_SYS_wait4 == __NR_wait4, "wait4 number");
_Static_assert(LINUX_SYS_renameat2 == __NR_renameat2,
	       "renameat2 number");
_Static_assert(LINUX_SYS_getrandom == __NR_getrandom,
	       "getrandom number");

_Static_assert(sizeof(struct linux_stat) == sizeof(struct stat),
	       "stat size");
_Static_assert(offsetof(struct linux_stat, size) ==
	       offsetof(struct stat, st_size), "stat size offset");
_Static_assert(offsetof(struct linux_stat, blocks) ==
	       offsetof(struct stat, st_blocks), "stat blocks offset");
_Static_assert(sizeof(struct linux_sigaction) == sizeof(struct sigaction),
	       "sigaction size");
_Static_assert(LINUX_SIGSET_SIZE == sizeof(sigset_t), "sigset size");
_Static_assert(sizeof(struct linux_sigaltstack) == sizeof(stack_t),
	       "signal alternate stack size");
_Static_assert(sizeof(struct linux_user_regs) ==
	       sizeof(struct user_regs_struct), "signal register size");
_Static_assert(sizeof(struct linux_sigcontext) ==
	       sizeof(struct sigcontext), "signal context size");
_Static_assert(sizeof(struct linux_ucontext) == sizeof(struct ucontext),
	       "user context size");
_Static_assert(offsetof(struct linux_ucontext, signal_mask) ==
	       offsetof(struct ucontext, uc_sigmask),
	       "user context signal mask offset");
_Static_assert(offsetof(struct linux_ucontext, mcontext) ==
	       offsetof(struct ucontext, uc_mcontext),
	       "user context machine state offset");
_Static_assert(LINUX_SIGHUP == SIGHUP, "SIGHUP value");
_Static_assert(LINUX_SIGKILL == SIGKILL, "SIGKILL value");
_Static_assert(LINUX_SIGCHLD == SIGCHLD, "SIGCHLD value");
_Static_assert(LINUX_SIGRTMIN == SIGRTMIN, "SIGRTMIN value");
_Static_assert(LINUX_SIGRTMAX == SIGRTMAX, "SIGRTMAX value");
_Static_assert(LINUX_SA_NOCLDSTOP == SA_NOCLDSTOP,
	       "SA_NOCLDSTOP value");
_Static_assert(LINUX_SA_SIGINFO == SA_SIGINFO, "SA_SIGINFO value");
_Static_assert(LINUX_SA_ONSTACK == SA_ONSTACK, "SA_ONSTACK value");
_Static_assert(LINUX_SA_RESTART == SA_RESTART, "SA_RESTART value");
_Static_assert(LINUX_SA_NODEFER == SA_NODEFER, "SA_NODEFER value");
_Static_assert(LINUX_SA_RESETHAND == SA_RESETHAND,
	       "SA_RESETHAND value");
_Static_assert(LINUX_BUS_ADRERR == BUS_ADRERR, "BUS_ADRERR value");
_Static_assert(sizeof(struct linux_termios) == sizeof(struct termios),
	       "termios size");
_Static_assert(sizeof(struct linux_winsize) == sizeof(struct winsize),
	       "winsize size");
_Static_assert(sizeof(struct linux_timespec) ==
	       sizeof(struct __kernel_timespec), "timespec size");
_Static_assert(sizeof(struct linux_pollfd) == sizeof(struct pollfd),
	       "pollfd size");
_Static_assert(offsetof(struct linux_pollfd, revents) ==
	       offsetof(struct pollfd, revents), "pollfd revents offset");
_Static_assert(sizeof(struct linux_sockaddr_in) ==
	       sizeof(struct sockaddr_in), "sockaddr_in size");
_Static_assert(offsetof(struct linux_sockaddr_in, port) ==
	       offsetof(struct sockaddr_in, sin_port),
	       "sockaddr_in port offset");
_Static_assert(LINUX_SOL_SOCKET == SOL_SOCKET, "SOL_SOCKET value");
_Static_assert(LINUX_SO_REUSEADDR == SO_REUSEADDR,
	       "SO_REUSEADDR value");
_Static_assert(LINUX_SO_RCVTIMEO == SO_RCVTIMEO,
	       "SO_RCVTIMEO value");
_Static_assert(LINUX_TCP_NODELAY == TCP_NODELAY, "TCP_NODELAY value");
_Static_assert(LINUX_TCGETS == TCGETS, "TCGETS value");
_Static_assert(LINUX_TCSETS == TCSETS, "TCSETS value");
_Static_assert(LINUX_TCSETSW == TCSETSW, "TCSETSW value");
_Static_assert(LINUX_TCSETSF == TCSETSF, "TCSETSF value");
_Static_assert(LINUX_TIOCGWINSZ == TIOCGWINSZ, "TIOCGWINSZ value");
_Static_assert(LINUX_PROT_NONE == PROT_NONE, "PROT_NONE value");
_Static_assert(LINUX_PROT_READ == PROT_READ, "PROT_READ value");
_Static_assert(LINUX_PROT_WRITE == PROT_WRITE, "PROT_WRITE value");
_Static_assert(LINUX_PROT_EXEC == PROT_EXEC, "PROT_EXEC value");
_Static_assert(LINUX_MAP_SHARED == MAP_SHARED, "MAP_SHARED value");
_Static_assert(LINUX_MS_ASYNC == MS_ASYNC, "MS_ASYNC value");
_Static_assert(LINUX_MS_INVALIDATE == MS_INVALIDATE,
	       "MS_INVALIDATE value");
_Static_assert(LINUX_MS_SYNC == MS_SYNC, "MS_SYNC value");
_Static_assert(LINUX_GRND_NONBLOCK == GRND_NONBLOCK,
	       "GRND_NONBLOCK value");
_Static_assert(LINUX_GRND_RANDOM == GRND_RANDOM, "GRND_RANDOM value");
_Static_assert(LINUX_GRND_INSECURE == GRND_INSECURE,
	       "GRND_INSECURE value");
_Static_assert(LINUX_MAP_PRIVATE == MAP_PRIVATE, "MAP_PRIVATE value");
_Static_assert(LINUX_MAP_FIXED == MAP_FIXED, "MAP_FIXED value");
_Static_assert(LINUX_MAP_ANONYMOUS == MAP_ANONYMOUS,
	       "MAP_ANONYMOUS value");
_Static_assert(LINUX_CLONE_VM == CLONE_VM, "CLONE_VM value");
_Static_assert(LINUX_CLONE_THREAD == CLONE_THREAD,
	       "CLONE_THREAD value");
_Static_assert(LINUX_CLONE_SETTLS == CLONE_SETTLS,
	       "CLONE_SETTLS value");
_Static_assert(LINUX_CLONE_PARENT_SETTID == CLONE_PARENT_SETTID,
	       "CLONE_PARENT_SETTID value");
_Static_assert(LINUX_CLONE_CHILD_CLEARTID == CLONE_CHILD_CLEARTID,
	       "CLONE_CHILD_CLEARTID value");
_Static_assert(LINUX_FUTEX_WAIT == FUTEX_WAIT, "FUTEX_WAIT value");
_Static_assert(LINUX_FUTEX_WAKE == FUTEX_WAKE, "FUTEX_WAKE value");
_Static_assert(LINUX_FUTEX_PRIVATE_FLAG == FUTEX_PRIVATE_FLAG,
	       "FUTEX_PRIVATE_FLAG value");
_Static_assert(LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED ==
	       MEMBARRIER_CMD_PRIVATE_EXPEDITED,
	       "MEMBARRIER_CMD_PRIVATE_EXPEDITED value");
