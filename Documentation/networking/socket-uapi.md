# Linux IPv4 socket UAPI

Caffeinix exposes Linux RISC-V syscall numbers and userspace layouts while
using lwIP internally.  `kernel/include/linux_uapi.h` owns the copied Linux
constants and structures; `make check-uapi` compares them at compile time
with installed RISC-V Linux headers.

Each userspace socket is an anonymous Caffeinix VFS file wrapping a private
lwIP socket.  lwIP descriptor numbers never enter userspace.  Ordinary
`read`, `write`, `close`, `dup`, `fcntl`, `fstat`, close-on-exec, and `ppoll`
therefore use normal VFS lifetime rules.

## Supported interface

The initial family is `AF_INET`.  Supported types are TCP stream, UDP
datagram, and raw ICMP sockets.  The implemented calls are:

- `socket`, `bind`, `listen`, `accept`, `accept4`, and `connect`;
- `getsockname`, `getpeername`, `shutdown`, and `socketpair` rejection;
- `sendto`, `recvfrom`, `sendmsg`, and `recvmsg` without ancillary data;
- `setsockopt` and `getsockopt` for the tested `SOL_SOCKET`, IP, and TCP
  subset; and
- `ppoll`, `FIONREAD`, `FIONBIO`, `SOCK_CLOEXEC`, `SOCK_NONBLOCK`, and
  `F_SETFL(O_NONBLOCK)`.

The current musl tests also use monotonic `clock_gettime`, `ftruncate`, and
process nice calls. Linux errors are translated explicitly; unsupported
families, flags, control messages, and options fail instead of reporting
false success.

Socket payload copies are bounded to one page per syscall.  TCP may return a
short read or write and userspace must retry.  An oversized datagram fails
with `EMSGSIZE`; an empty `sendmsg` still emits an empty datagram.  Connected
UDP and raw sockets implement read and write shutdown in the wrapper because
lwIP only exposes shutdown for TCP.  UDP and raw sockets accept `AF_UNSPEC`
through `connect` to remove their peer association.  Raw receive addresses
report a zero port as required by Linux. `ppoll` accepts an optional temporary
signal mask. It restores the old mask on normal completion; after interruption,
the signal-delivery/return path preserves and restores the saved mask. A caught
signal interrupts the wait with `EINTR`, including handlers using `SA_RESTART`.

## QEMU networking

The normal kernel QEMU target defaults to `NET_BACKEND=user` and attaches a
NIC. Set `NET_BACKEND=` for an offline boot. For example, explicit networking
settings are:

```bash
make qemu \
  FS_IMG=/absolute/path/to/caffeinix.ext4 \
  NET_BACKEND=user \
  NET_BUS=virtio-mmio-bus.2 \
  NET_MAC=52:54:00:12:34:56
```

The defaults place storage on buses 0 and 1 and the optional NIC on bus 2.
Tests deliberately reorder these devices to ensure drivers do not depend on
transport enumeration order.

`make -C tests qemu` starts local UDP, TCP, reverse-TCP, bulk-transfer, and
HTTP fixtures.  It verifies DHCP, ICMP to the QEMU gateway, UDP, TCP client
and server paths, transfers larger than the socket and packet buffers,
BusyBox `nc`, `wget`, and `ping`, polling, no-NIC boots, and a NIC with no DHCP
peer. The signal runtime test covers `ppoll` mask restoration and interruption.
No test requires Internet access or a privileged TAP device.

## Deliberate omissions

IPv6, AF_UNIX, packet sockets, netlink, ancillary data, credentials,
`pselect6`, asynchronous I/O, and the full Linux socket-option surface are not
implemented. Direct blocking lwIP socket calls still use non-interruptible
semaphore/mailbox waits; this limitation does not apply to VFS `ppoll` waits.

## Error and lifetime rules

The socket layer returns Linux negative errno values to the syscall layer.
It maps lwIP errors explicitly and treats unknown internal failures as
`EIO`.  Close removes the VFS file from the socket registry before releasing
the lwIP descriptor, so procfs snapshots cannot retain a freed wrapper.
`accept4` closes a newly installed file if copying the peer address back to
userspace fails.  Socket payload buffers are temporary kernel pages; no
userspace pointer is retained after the syscall returns.

`ppoll` relies on lwIP socket event notifications reaching the VFS poll wait
queue and on signals waking interruptible waiters. Direct blocking socket
operations instead wait inside lwIP. Applications needing signal-driven
cancellation should combine nonblocking socket I/O with `ppoll`, rather than
assume that a signal interrupts every blocking lwIP call.
