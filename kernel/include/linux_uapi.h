#ifndef __CAFFEINIX_KERNEL_LINUX_UAPI_H
#define __CAFFEINIX_KERNEL_LINUX_UAPI_H

#include <typedefs.h>

/* Linux RISC-V uses the asm-generic syscall number space. */
#define LINUX_SYS_getcwd             17
#define LINUX_SYS_dup                23
#define LINUX_SYS_dup3               24
#define LINUX_SYS_fcntl              25
#define LINUX_SYS_ioctl              29
#define LINUX_SYS_mkdirat            34
#define LINUX_SYS_unlinkat           35
#define LINUX_SYS_symlinkat          36
#define LINUX_SYS_linkat             37
#define LINUX_SYS_ftruncate          46
#define LINUX_SYS_faccessat          48
#define LINUX_SYS_chdir              49
#define LINUX_SYS_openat             56
#define LINUX_SYS_close              57
#define LINUX_SYS_pipe2              59
#define LINUX_SYS_getdents64         61
#define LINUX_SYS_lseek              62
#define LINUX_SYS_read               63
#define LINUX_SYS_write              64
#define LINUX_SYS_readv              65
#define LINUX_SYS_writev             66
#define LINUX_SYS_pread64            67
#define LINUX_SYS_pwrite64           68
#define LINUX_SYS_preadv             69
#define LINUX_SYS_pwritev            70
#define LINUX_SYS_sendfile           71
#define LINUX_SYS_ppoll              73
#define LINUX_SYS_readlinkat         78
#define LINUX_SYS_newfstatat         79
#define LINUX_SYS_fstat              80
#define LINUX_SYS_sync               81
#define LINUX_SYS_fsync              82
#define LINUX_SYS_fdatasync          83
#define LINUX_SYS_utimensat          88
#define LINUX_SYS_exit               93
#define LINUX_SYS_exit_group         94
#define LINUX_SYS_set_tid_address    96
#define LINUX_SYS_futex              98
#define LINUX_SYS_set_robust_list    99
#define LINUX_SYS_get_robust_list   100
#define LINUX_SYS_nanosleep         101
#define LINUX_SYS_getitimer         102
#define LINUX_SYS_setitimer         103
#define LINUX_SYS_clock_gettime     113
#define LINUX_SYS_clock_getres      114
#define LINUX_SYS_clock_nanosleep   115
#define LINUX_SYS_sched_getaffinity 123
#define LINUX_SYS_kill              129
#define LINUX_SYS_tkill             130
#define LINUX_SYS_tgkill            131
#define LINUX_SYS_sigaltstack       132
#define LINUX_SYS_rt_sigsuspend     133
#define LINUX_SYS_rt_sigaction      134
#define LINUX_SYS_rt_sigprocmask    135
#define LINUX_SYS_rt_sigpending     136
#define LINUX_SYS_rt_sigtimedwait   137
#define LINUX_SYS_rt_sigreturn      139
#define LINUX_SYS_setpriority       140
#define LINUX_SYS_getpriority       141
#define LINUX_SYS_setregid          143
#define LINUX_SYS_setgid            144
#define LINUX_SYS_setreuid          145
#define LINUX_SYS_setuid            146
#define LINUX_SYS_setresuid         147
#define LINUX_SYS_getresuid         148
#define LINUX_SYS_setresgid         149
#define LINUX_SYS_getresgid         150
#define LINUX_SYS_setfsuid          151
#define LINUX_SYS_setfsgid          152
#define LINUX_SYS_setpgid           154
#define LINUX_SYS_getpgid           155
#define LINUX_SYS_getsid            156
#define LINUX_SYS_setsid            157
#define LINUX_SYS_getgroups         158
#define LINUX_SYS_setgroups         159
#define LINUX_SYS_uname             160
#define LINUX_SYS_umask             166
#define LINUX_SYS_prctl             167
#define LINUX_SYS_gettimeofday     169
#define LINUX_SYS_getpid            172
#define LINUX_SYS_getppid           173
#define LINUX_SYS_getuid            174
#define LINUX_SYS_geteuid           175
#define LINUX_SYS_getgid            176
#define LINUX_SYS_getegid           177
#define LINUX_SYS_gettid            178
#define LINUX_SYS_sysinfo           179
#define LINUX_SYS_socket            198
#define LINUX_SYS_socketpair        199
#define LINUX_SYS_bind              200
#define LINUX_SYS_listen            201
#define LINUX_SYS_accept            202
#define LINUX_SYS_connect           203
#define LINUX_SYS_getsockname       204
#define LINUX_SYS_getpeername       205
#define LINUX_SYS_sendto            206
#define LINUX_SYS_recvfrom          207
#define LINUX_SYS_setsockopt        208
#define LINUX_SYS_getsockopt        209
#define LINUX_SYS_shutdown          210
#define LINUX_SYS_sendmsg           211
#define LINUX_SYS_recvmsg           212
#define LINUX_SYS_brk               214
#define LINUX_SYS_munmap            215
#define LINUX_SYS_clone             220
#define LINUX_SYS_execve            221
#define LINUX_SYS_mmap              222
#define LINUX_SYS_mprotect          226
#define LINUX_SYS_msync             227
#define LINUX_SYS_accept4           242
#define LINUX_SYS_riscv_flush_icache 259
#define LINUX_SYS_wait4             260
#define LINUX_SYS_renameat2         276
#define LINUX_SYS_getrandom         278
#define LINUX_SYS_membarrier        283
#define LINUX_SYS_preadv2           286
#define LINUX_SYS_pwritev2          287

#define LINUX_SYS_RISCV_FLUSH_ICACHE_LOCAL 1UL
#define LINUX_SYS_RISCV_FLUSH_ICACHE_ALL \
	LINUX_SYS_RISCV_FLUSH_ICACHE_LOCAL

#define LINUX_F_OK                    0
#define LINUX_X_OK                    1
#define LINUX_W_OK                    2
#define LINUX_R_OK                    4

#define LINUX_EPERM                   1
#define LINUX_ENOENT                  2
#define LINUX_ESRCH                   3
#define LINUX_EINTR                   4
#define LINUX_EIO                     5
#define LINUX_ENXIO                   6
#define LINUX_E2BIG                   7
#define LINUX_ENOEXEC                 8
#define LINUX_EBADF                   9
#define LINUX_ECHILD                 10
#define LINUX_EAGAIN                 11
#define LINUX_ENOMEM                 12
#define LINUX_EACCES                 13
#define LINUX_EFAULT                 14
#define LINUX_EBUSY                  16
#define LINUX_EEXIST                 17
#define LINUX_EXDEV                  18
#define LINUX_ENODEV                 19
#define LINUX_ENOTDIR                20
#define LINUX_EISDIR                 21
#define LINUX_EINVAL                 22
#define LINUX_ENFILE                 23
#define LINUX_EMFILE                 24
#define LINUX_ENOTTY                 25
#define LINUX_ETXTBSY                26
#define LINUX_EFBIG                  27
#define LINUX_ENOSPC                 28
#define LINUX_ESPIPE                 29
#define LINUX_EROFS                  30
#define LINUX_EMLINK                 31
#define LINUX_EPIPE                  32
#define LINUX_ERANGE                 34
#define LINUX_ENAMETOOLONG           36
#define LINUX_ENOSYS                 38
#define LINUX_ENOTEMPTY              39
#define LINUX_ELOOP                  40
#define LINUX_EOVERFLOW              75
#define LINUX_ENOTSOCK               88
#define LINUX_EDESTADDRREQ           89
#define LINUX_EMSGSIZE               90
#define LINUX_EPROTOTYPE             91
#define LINUX_ENOPROTOOPT            92
#define LINUX_EPROTONOSUPPORT        93
#define LINUX_ESOCKTNOSUPPORT        94
#define LINUX_EOPNOTSUPP             95
#define LINUX_EAFNOSUPPORT           97
#define LINUX_EADDRINUSE             98
#define LINUX_EADDRNOTAVAIL          99
#define LINUX_ENETDOWN              100
#define LINUX_ENETUNREACH           101
#define LINUX_ENETRESET             102
#define LINUX_ECONNABORTED          103
#define LINUX_ECONNRESET            104
#define LINUX_ENOBUFS               105
#define LINUX_EISCONN               106
#define LINUX_ENOTCONN              107
#define LINUX_ESHUTDOWN             108
#define LINUX_ETIMEDOUT             110
#define LINUX_ECONNREFUSED          111
#define LINUX_EHOSTUNREACH          113
#define LINUX_EALREADY              114
#define LINUX_EINPROGRESS           115

#define LINUX_IOV_MAX             1024
#define LINUX_RWF_NOAPPEND         0x20

struct linux_iovec {
	uint64 base;
	uint64 len;
};

struct linux_timespec {
	int64 seconds;
	int64 nanoseconds;
};

struct linux_timeval {
	int64 seconds;
	int64 microseconds;
};

struct linux_timezone {
	int32 minutes_west;
	int32 dst_time;
};

struct linux_itimerval {
	struct linux_timeval interval;
	struct linux_timeval value;
};

#define LINUX_UTS_LEN 65

struct linux_utsname {
	char sysname[LINUX_UTS_LEN];
	char nodename[LINUX_UTS_LEN];
	char release[LINUX_UTS_LEN];
	char version[LINUX_UTS_LEN];
	char machine[LINUX_UTS_LEN];
	char domainname[LINUX_UTS_LEN];
};

struct linux_sysinfo {
	int64 uptime;
	uint64 loads[3];
	uint64 totalram;
	uint64 freeram;
	uint64 sharedram;
	uint64 bufferram;
	uint64 totalswap;
	uint64 freeswap;
	uint16 procs;
	uint16 pad;
	uint32 alignment;
	uint64 totalhigh;
	uint64 freehigh;
	uint32 mem_unit;
	uint32 reserved;
};

#define LINUX_SI_LOAD_SHIFT 16

struct linux_pollfd {
	int32 fd;
	int16 events;
	int16 revents;
};

struct linux_sockaddr {
	uint16 family;
	uint8 data[14];
};

struct linux_sockaddr_in {
	uint16 family;
	uint16 port;
	uint32 address;
	uint8 zero[8];
};

struct linux_msghdr {
	uint64 name;
	uint32 name_length;
	uint32 pad1;
	uint64 iov;
	uint64 iov_length;
	uint64 control;
	uint64 control_length;
	uint32 flags;
	uint32 pad2;
};

struct linux_linger {
	int32 enabled;
	int32 seconds;
};

#define LINUX_AT_FDCWD              -100
#define LINUX_AT_SYMLINK_NOFOLLOW  0x100
#define LINUX_AT_REMOVEDIR         0x200
#define LINUX_AT_SYMLINK_FOLLOW    0x400
#define LINUX_AT_EMPTY_PATH       0x1000

#define LINUX_UTIME_NOW      1073741823L
#define LINUX_UTIME_OMIT     1073741822L

#define LINUX_RENAME_NOREPLACE       0x1

#define LINUX_O_ACCMODE          00000003
#define LINUX_O_RDONLY          00000000
#define LINUX_O_WRONLY          00000001
#define LINUX_O_RDWR            00000002
#define LINUX_O_CREAT           00000100
#define LINUX_O_EXCL            00000200
#define LINUX_O_NOCTTY          00000400
#define LINUX_O_TRUNC           00001000
#define LINUX_O_APPEND          00002000
#define LINUX_O_NONBLOCK        00004000
#define LINUX_O_LARGEFILE       00100000
#define LINUX_O_DIRECTORY       00200000
#define LINUX_O_CLOEXEC         02000000

#define LINUX_AF_UNSPEC                  0
#define LINUX_AF_INET                    2

#define LINUX_SOCK_STREAM                1
#define LINUX_SOCK_DGRAM                 2
#define LINUX_SOCK_RAW                   3
#define LINUX_SOCK_NONBLOCK      LINUX_O_NONBLOCK
#define LINUX_SOCK_CLOEXEC        LINUX_O_CLOEXEC

#define LINUX_IPPROTO_IP                 0
#define LINUX_IPPROTO_ICMP               1
#define LINUX_IPPROTO_TCP                6
#define LINUX_IPPROTO_UDP               17

#define LINUX_MSG_OOB                 0x1
#define LINUX_MSG_PEEK                0x2
#define LINUX_MSG_DONTROUTE           0x4
#define LINUX_MSG_TRUNC              0x20
#define LINUX_MSG_DONTWAIT           0x40
#define LINUX_MSG_WAITALL           0x100
#define LINUX_MSG_NOSIGNAL         0x4000
#define LINUX_MSG_MORE             0x8000

#define LINUX_SOL_SOCKET                  1
#define LINUX_SO_REUSEADDR                2
#define LINUX_SO_TYPE                     3
#define LINUX_SO_ERROR                    4
#define LINUX_SO_BROADCAST                6
#define LINUX_SO_RCVBUF                   8
#define LINUX_SO_KEEPALIVE                9
#define LINUX_SO_LINGER                  13
#define LINUX_SO_RCVTIMEO                20
#define LINUX_SO_SNDTIMEO                21
#define LINUX_SO_ACCEPTCONN              30

#define LINUX_IP_TTL                      2
#define LINUX_TCP_NODELAY                 1

#define LINUX_SHUT_RD                     0
#define LINUX_SHUT_WR                     1
#define LINUX_SHUT_RDWR                   2

#define LINUX_POLLIN                 0x001
#define LINUX_POLLOUT                0x004
#define LINUX_POLLERR                0x008
#define LINUX_POLLHUP                0x010
#define LINUX_POLLNVAL               0x020

#define LINUX_FIONREAD              0x541b
#define LINUX_FIONBIO               0x5421

#define LINUX_F_DUPFD                  0
#define LINUX_F_GETFD                  1
#define LINUX_F_SETFD                  2
#define LINUX_F_GETFL                  3
#define LINUX_F_SETFL                  4
#define LINUX_F_DUPFD_CLOEXEC       1030
#define LINUX_FD_CLOEXEC               1

#define LINUX_SEEK_SET                 0
#define LINUX_SEEK_CUR                 1
#define LINUX_SEEK_END                 2

#define LINUX_PROT_NONE                0
#define LINUX_PROT_READ              0x1
#define LINUX_PROT_WRITE             0x2
#define LINUX_PROT_EXEC              0x4
#define LINUX_MAP_SHARED             0x1
#define LINUX_MAP_PRIVATE            0x2
#define LINUX_MAP_FIXED             0x10
#define LINUX_MAP_ANONYMOUS         0x20
#define LINUX_MAP_NORESERVE       0x4000
#define LINUX_MAP_POPULATE        0x8000
#define LINUX_MAP_STACK          0x20000
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

#define LINUX_MS_ASYNC                0x1
#define LINUX_MS_INVALIDATE           0x2
#define LINUX_MS_SYNC                 0x4

#define LINUX_GRND_NONBLOCK            0x1
#define LINUX_GRND_RANDOM              0x2
#define LINUX_GRND_INSECURE            0x4

#define LINUX_PR_GET_NAME             16

#define LINUX_TCGETS              0x5401
#define LINUX_TCSETS              0x5402
#define LINUX_TCSETSW             0x5403
#define LINUX_TCSETSF             0x5404
#define LINUX_TIOCGPGRP           0x540f
#define LINUX_TIOCSPGRP           0x5410
#define LINUX_TIOCGWINSZ          0x5413
#define LINUX_TIOCGSID            0x5429

#define LINUX_ICRNL                0x100
#define LINUX_B38400                 0xf
#define LINUX_CS8                   0x30
#define LINUX_CREAD                 0x80
#define LINUX_CLOCAL               0x800
#define LINUX_ISIG                   0x1
#define LINUX_ICANON                 0x2
#define LINUX_ECHO                   0x8
#define LINUX_ECHOE                 0x10
#define LINUX_ECHOK                 0x20
#define LINUX_NOFLSH                0x80
#define LINUX_TOSTOP               0x100
#define LINUX_VINTR                    0
#define LINUX_VQUIT                    1
#define LINUX_VERASE                   2
#define LINUX_VEOF                     4
#define LINUX_VMIN                     6
#define LINUX_VSUSP                   10

#define LINUX_SIG_BLOCK                0
#define LINUX_SIG_UNBLOCK              1
#define LINUX_SIG_SETMASK              2
#define LINUX_SIGHUP                    1
#define LINUX_SIGINT                    2
#define LINUX_SIGQUIT                   3
#define LINUX_SIGILL                    4
#define LINUX_SIGTRAP                   5
#define LINUX_SIGABRT                   6
#define LINUX_SIGBUS                    7
#define LINUX_SIGFPE                    8
#define LINUX_SIGKILL                  9
#define LINUX_SIGUSR1                  10
#define LINUX_SIGSEGV                  11
#define LINUX_SIGUSR2                  12
#define LINUX_SIGPIPE                  13
#define LINUX_SIGALRM                  14
#define LINUX_SIGTERM                  15
#define LINUX_SIGSTKFLT                16
#define LINUX_SIGCHLD                 17
#define LINUX_SIGCONT                 18
#define LINUX_SIGSTOP                 19
#define LINUX_SIGTSTP                 20
#define LINUX_SIGTTIN                 21
#define LINUX_SIGTTOU                 22
#define LINUX_SIGURG                  23
#define LINUX_SIGXCPU                 24
#define LINUX_SIGXFSZ                 25
#define LINUX_SIGVTALRM               26
#define LINUX_SIGPROF                 27
#define LINUX_SIGWINCH                28
#define LINUX_SIGIO                   29
#define LINUX_SIGPWR                  30
#define LINUX_SIGSYS                  31
#define LINUX_SIGRTMIN                32
#define LINUX_SIGRTMAX                64
#define LINUX_SIGSET_SIZE              8

#define LINUX_SIG_DFL                   0
#define LINUX_SIG_IGN                   1

#define LINUX_SA_NOCLDSTOP     0x00000001U
#define LINUX_SA_NOCLDWAIT     0x00000002U
#define LINUX_SA_SIGINFO       0x00000004U
#define LINUX_SA_ONSTACK       0x08000000U
#define LINUX_SA_RESTART       0x10000000U
#define LINUX_SA_NODEFER       0x40000000U
#define LINUX_SA_RESETHAND     0x80000000U

#define LINUX_SS_ONSTACK                1
#define LINUX_SS_DISABLE                2
#define LINUX_SS_AUTODISARM    0x80000000U

#define LINUX_SI_USER                    0
#define LINUX_SI_KERNEL               128
#define LINUX_SI_TKILL                 -6

#define LINUX_ILL_ILLOPC                 1
#define LINUX_TRAP_BRKPT                 1
#define LINUX_BUS_ADRALN                 1
#define LINUX_BUS_ADRERR                 2
#define LINUX_SEGV_MAPERR                1
#define LINUX_SEGV_ACCERR                2

#define LINUX_CLD_EXITED                 1
#define LINUX_CLD_KILLED                 2
#define LINUX_CLD_DUMPED                 3
#define LINUX_CLD_STOPPED                5
#define LINUX_CLD_CONTINUED              6

#define LINUX_CLONE_VM             0x100
#define LINUX_CLONE_FS             0x200
#define LINUX_CLONE_FILES          0x400
#define LINUX_CLONE_SIGHAND        0x800
#define LINUX_CLONE_VFORK         0x4000
#define LINUX_CLONE_THREAD       0x10000
#define LINUX_CLONE_SYSVSEM      0x40000
#define LINUX_CLONE_SETTLS       0x80000
#define LINUX_CLONE_PARENT_SETTID 0x100000
#define LINUX_CLONE_CHILD_CLEARTID 0x200000
#define LINUX_CLONE_DETACHED     0x400000
#define LINUX_CLONE_CHILD_SETTID 0x1000000
#define LINUX_CLONE_SIGNAL_MASK     0xff

#define LINUX_FUTEX_WAIT                 0
#define LINUX_FUTEX_WAKE                 1
#define LINUX_FUTEX_REQUEUE              3
#define LINUX_FUTEX_CMP_REQUEUE          4
#define LINUX_FUTEX_WAIT_BITSET          9
#define LINUX_FUTEX_WAKE_BITSET         10
#define LINUX_FUTEX_PRIVATE_FLAG       128
#define LINUX_FUTEX_CLOCK_REALTIME     256
#define LINUX_FUTEX_CMD_MASK           127
#define LINUX_FUTEX_BITSET_MATCH_ANY 0xffffffffU
#define LINUX_FUTEX_WAITERS          0x80000000U
#define LINUX_FUTEX_OWNER_DIED       0x40000000U

#define LINUX_MEMBARRIER_CMD_QUERY                         0
#define LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED             8
#define LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED   16

#define LINUX_WNOHANG                   1
#define LINUX_WUNTRACED                 2
#define LINUX_WCONTINUED                8

#define LINUX_CLOCK_REALTIME             0
#define LINUX_CLOCK_MONOTONIC            1
#define LINUX_CLOCK_BOOTTIME             7
#define LINUX_TIMER_ABSTIME            0x1
#define LINUX_ITIMER_REAL                0

#define LINUX_PRIO_PROCESS               0

#define LINUX_S_IFMT             00170000
#define LINUX_S_IFREG            0100000
#define LINUX_S_IFDIR            0040000
#define LINUX_S_IFCHR            0020000
#define LINUX_S_IFBLK            0060000
#define LINUX_S_IFIFO            0010000
#define LINUX_S_IFLNK            0120000
#define LINUX_S_IFSOCK           0140000

#define LINUX_DT_UNKNOWN               0
#define LINUX_DT_CHR                   2
#define LINUX_DT_DIR                   4
#define LINUX_DT_REG                   8

struct linux_stat {
	uint64 dev;
	uint64 ino;
	uint32 mode;
	uint32 nlink;
	uint32 uid;
	uint32 gid;
	uint64 rdev;
	uint64 pad1;
	int64 size;
	int32 blksize;
	int32 pad2;
	int64 blocks;
	int64 atime;
	uint64 atime_nsec;
	int64 mtime;
	uint64 mtime_nsec;
	int64 ctime;
	uint64 ctime_nsec;
	uint32 unused4;
	uint32 unused5;
};

struct linux_sigaction {
	uint64 handler;
	uint64 flags;
	uint64 mask;
};

struct linux_sigaltstack {
	uint64 sp;
	int32 flags;
	uint32 padding;
	uint64 size;
};

struct linux_user_regs {
	uint64 pc;
	uint64 ra;
	uint64 sp;
	uint64 gp;
	uint64 tp;
	uint64 t0;
	uint64 t1;
	uint64 t2;
	uint64 s0;
	uint64 s1;
	uint64 a0;
	uint64 a1;
	uint64 a2;
	uint64 a3;
	uint64 a4;
	uint64 a5;
	uint64 a6;
	uint64 a7;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
	uint64 t3;
	uint64 t4;
	uint64 t5;
	uint64 t6;
};

struct linux_riscv_d_ext_state {
	uint64 f[32];
	uint32 fcsr;
};

struct linux_riscv_ctx_header {
	uint32 magic;
	uint32 size;
};

struct linux_riscv_extra_ext_header {
	uint32 padding[129] __attribute__((aligned(16)));
	uint32 reserved;
	struct linux_riscv_ctx_header header;
};

union linux_riscv_fp_state {
	struct linux_riscv_d_ext_state d;
	struct linux_riscv_extra_ext_header ext;
} __attribute__((aligned(16)));

struct linux_sigcontext {
	struct linux_user_regs regs;
	union linux_riscv_fp_state fpregs;
} __attribute__((aligned(16)));

struct linux_ucontext {
	uint64 flags;
	uint64 link;
	struct linux_sigaltstack stack;
	uint64 signal_mask;
	uint8 unused[120];
	struct linux_sigcontext mcontext;
} __attribute__((aligned(16)));

struct linux_siginfo {
	int32 signal;
	int32 error;
	int32 code;
	int32 padding;
	union {
		struct {
			int32 pid;
			uint32 uid;
		} kill;
		struct {
			int32 pid;
			uint32 uid;
			int32 status;
			uint32 padding;
			int64 user_time;
			int64 system_time;
		} child;
		struct {
			uint64 address;
		} fault;
		uint8 bytes[112];
	} fields;
};

struct linux_rt_sigframe {
	struct linux_siginfo info;
	struct linux_ucontext context;
};

struct linux_termios {
	uint32 iflag;
	uint32 oflag;
	uint32 cflag;
	uint32 lflag;
	uint8 line;
	uint8 control[19];
};

struct linux_winsize {
	uint16 rows;
	uint16 columns;
	uint16 xpixel;
	uint16 ypixel;
};

_Static_assert(sizeof(struct linux_iovec) == 16,
	       "Linux iovec layout changed");
_Static_assert(sizeof(struct linux_timespec) == 16,
	       "Linux timespec layout changed");
_Static_assert(sizeof(struct linux_timeval) == 16,
	       "Linux timeval layout changed");
_Static_assert(sizeof(struct linux_pollfd) == 8,
	       "Linux pollfd layout changed");
_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
	       "Linux sockaddr_in layout changed");
_Static_assert(sizeof(struct linux_msghdr) == 56,
	       "Linux msghdr layout changed");
_Static_assert(sizeof(struct linux_stat) == 128,
	       "Linux RISC-V stat layout changed");
_Static_assert(__builtin_offsetof(struct linux_stat, size) == 48,
	       "Linux RISC-V stat offsets changed");
_Static_assert(sizeof(struct linux_sigaction) == 24,
	       "Linux RISC-V sigaction layout changed");
_Static_assert(sizeof(struct linux_sigaltstack) == 24,
	       "Linux RISC-V sigaltstack layout changed");
_Static_assert(sizeof(struct linux_user_regs) == 256,
	       "Linux RISC-V register layout changed");
_Static_assert(sizeof(union linux_riscv_fp_state) == 528,
	       "Linux RISC-V FP state layout changed");
_Static_assert(sizeof(struct linux_sigcontext) == 784,
	       "Linux RISC-V sigcontext layout changed");
_Static_assert(__builtin_offsetof(struct linux_ucontext, mcontext) == 176,
	       "Linux RISC-V ucontext offsets changed");
_Static_assert(sizeof(struct linux_ucontext) == 960,
	       "Linux RISC-V ucontext layout changed");
_Static_assert(sizeof(struct linux_siginfo) == 128,
	       "Linux RISC-V siginfo layout changed");
_Static_assert(sizeof(struct linux_rt_sigframe) == 1088,
	       "Linux RISC-V signal frame layout changed");
_Static_assert(sizeof(struct linux_termios) == 36,
	       "Linux RISC-V termios layout changed");
_Static_assert(sizeof(struct linux_winsize) == 8,
	       "Linux winsize layout changed");

/* ELF auxiliary vector tags used by static musl startup. */
#define LINUX_AT_NULL                0
#define LINUX_AT_PHDR                3
#define LINUX_AT_PHENT               4
#define LINUX_AT_PHNUM               5
#define LINUX_AT_PAGESZ              6
#define LINUX_AT_BASE                7
#define LINUX_AT_ENTRY               9
#define LINUX_AT_UID                11
#define LINUX_AT_EUID               12
#define LINUX_AT_GID                13
#define LINUX_AT_EGID               14
#define LINUX_AT_SECURE             23
#define LINUX_AT_RANDOM             25
#define LINUX_AT_EXECFN             31

#endif
