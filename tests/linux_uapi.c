/* Linux 6.10 RISC-V UAPI is the recorded compatibility baseline. */
#include <stddef.h>

#include <linux_uapi.h>

#include <asm/ioctls.h>
#include <asm/signal.h>
#include <asm/stat.h>
#include <asm/termbits.h>
#include <asm/unistd.h>
#include <asm-generic/termios.h>
#include <linux/time_types.h>

_Static_assert(LINUX_SYS_getcwd == __NR_getcwd, "getcwd number");
_Static_assert(LINUX_SYS_openat == __NR_openat, "openat number");
_Static_assert(LINUX_SYS_symlinkat == __NR_symlinkat,
	       "symlinkat number");
_Static_assert(LINUX_SYS_linkat == __NR_linkat, "linkat number");
_Static_assert(LINUX_SYS_getdents64 == __NR_getdents64,
	       "getdents64 number");
_Static_assert(LINUX_SYS_readlinkat == __NR_readlinkat,
	       "readlinkat number");
_Static_assert(LINUX_SYS_sync == __NR_sync, "sync number");
_Static_assert(LINUX_SYS_fsync == __NR_fsync, "fsync number");
_Static_assert(LINUX_SYS_umask == __NR_umask, "umask number");
_Static_assert(LINUX_SYS_rt_sigaction == __NR_rt_sigaction,
	       "rt_sigaction number");
_Static_assert(LINUX_SYS_clock_gettime == __NR_clock_gettime,
	       "clock_gettime number");
_Static_assert(LINUX_SYS_clone == __NR_clone, "clone number");
_Static_assert(LINUX_SYS_execve == __NR_execve, "execve number");
_Static_assert(LINUX_SYS_wait4 == __NR_wait4, "wait4 number");
_Static_assert(LINUX_SYS_renameat2 == __NR_renameat2,
	       "renameat2 number");

_Static_assert(sizeof(struct linux_stat) == sizeof(struct stat),
	       "stat size");
_Static_assert(offsetof(struct linux_stat, size) ==
	       offsetof(struct stat, st_size), "stat size offset");
_Static_assert(offsetof(struct linux_stat, blocks) ==
	       offsetof(struct stat, st_blocks), "stat blocks offset");
_Static_assert(sizeof(struct linux_sigaction) == sizeof(struct sigaction),
	       "sigaction size");
_Static_assert(LINUX_SIGSET_SIZE == sizeof(sigset_t), "sigset size");
_Static_assert(sizeof(struct linux_termios) == sizeof(struct termios),
	       "termios size");
_Static_assert(sizeof(struct linux_winsize) == sizeof(struct winsize),
	       "winsize size");
_Static_assert(sizeof(struct linux_timespec) ==
	       sizeof(struct __kernel_timespec), "timespec size");
_Static_assert(LINUX_TCGETS == TCGETS, "TCGETS value");
_Static_assert(LINUX_TCSETS == TCSETS, "TCSETS value");
_Static_assert(LINUX_TCSETSW == TCSETSW, "TCSETSW value");
_Static_assert(LINUX_TCSETSF == TCSETSF, "TCSETSF value");
_Static_assert(LINUX_TIOCGWINSZ == TIOCGWINSZ, "TIOCGWINSZ value");
