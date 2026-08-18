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
#define LINUX_SYS_writev             66
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
#define LINUX_SYS_clock_gettime     113
#define LINUX_SYS_rt_sigaction      134
#define LINUX_SYS_rt_sigprocmask    135
#define LINUX_SYS_setpriority       140
#define LINUX_SYS_getpriority       141
#define LINUX_SYS_umask             166
#define LINUX_SYS_prctl             167
#define LINUX_SYS_getpid            172
#define LINUX_SYS_getppid           173
#define LINUX_SYS_getuid            174
#define LINUX_SYS_geteuid           175
#define LINUX_SYS_getgid            176
#define LINUX_SYS_getegid           177
#define LINUX_SYS_gettid            178
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
#define LINUX_SYS_accept4           242
#define LINUX_SYS_wait4             260
#define LINUX_SYS_renameat2         276

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

#define LINUX_IOV_MAX               16

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

#define LINUX_RENAME_NOREPLACE       0x1

#define LINUX_O_ACCMODE          00000003
#define LINUX_O_RDONLY          00000000
#define LINUX_O_WRONLY          00000001
#define LINUX_O_RDWR            00000002
#define LINUX_O_CREAT           00000100
#define LINUX_O_EXCL            00000200
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

#define LINUX_PR_GET_NAME             16

#define LINUX_TCGETS              0x5401
#define LINUX_TCSETS              0x5402
#define LINUX_TCSETSW             0x5403
#define LINUX_TCSETSF             0x5404
#define LINUX_TIOCGWINSZ          0x5413

#define LINUX_ICRNL                0x100
#define LINUX_B38400                 0xf
#define LINUX_CS8                   0x30
#define LINUX_CREAD                 0x80
#define LINUX_CLOCAL               0x800
#define LINUX_ICANON                 0x2
#define LINUX_ECHO                   0x8
#define LINUX_ECHOE                 0x10
#define LINUX_ECHOK                 0x20
#define LINUX_VINTR                    0
#define LINUX_VERASE                   2
#define LINUX_VEOF                     4
#define LINUX_VMIN                     6

#define LINUX_SIG_BLOCK                0
#define LINUX_SIG_UNBLOCK              1
#define LINUX_SIG_SETMASK              2
#define LINUX_SIGKILL                  9
#define LINUX_SIGCHLD                 17
#define LINUX_SIGSTOP                 19
#define LINUX_SIGSET_SIZE              8

#define LINUX_CLONE_VM             0x100
#define LINUX_CLONE_VFORK         0x4000
#define LINUX_CLONE_SIGNAL_MASK     0xff

#define LINUX_WNOHANG                   1

#define LINUX_CLOCK_REALTIME             0
#define LINUX_CLOCK_MONOTONIC            1

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
