# lwIP port

Caffeinix vendors lwIP 2.2.1 from the upstream
`STABLE-2_2_1_RELEASE` tag at commit
`77dcd25a72509eb83f72b033d219b1d40cd8eb95`.  The source URL, imported paths,
and license are recorded in `net/lwip/UPSTREAM`; the BSD-3-Clause license is
preserved in `net/lwip/upstream/COPYING`.

Normal kernel builds use only the pinned tree and never download lwIP.  musl
and BusyBox remain external userspace and test inputs.

## Threading model

The port uses `NO_SYS=0`.  lwIP creates its `tcpip` kernel thread through
`sys_thread_new()` and processes protocol input and timers there.  NIC IRQs
schedule a Caffeinix work item; the worker copies the frame and calls
`tcpip_input()`.  Core-locking input is disabled, so no driver or hard IRQ
executes the lwIP core directly.

`sys_arch.c` maps lwIP mutexes, semaphores, mailboxes, lightweight protection,
time, and thread creation onto Caffeinix primitives.  Timed waits sleep on the
global monotonic RISC-V time counter; they do not poll.  The lightweight
protection lock disables preemption while held, so its per-CPU nesting depth
cannot migrate between acquire and release.

The port enables Ethernet, ARP, IPv4, ICMP, raw IP, DHCP, DNS, UDP, TCP,
netconn, and lwIP sockets.  IPv6 and lwIP application servers are disabled.
Pool and window sizes are explicit in `net/lwip/port/include/lwipopts.h`.

## Interface adapter

At initialization the adapter attaches each registered Caffeinix network
device to one lwIP `netif`.  Loopback receives `127.0.0.1/8`.  The first
Ethernet interface becomes the default and starts DHCP asynchronously; boot
does not wait for a lease.  Link and administrative state are forwarded to
the TCP/IP thread with `tcpip_callback()`.

The adapter contains no QEMU address constants.  QEMU user networking
normally supplies `10.0.2.x`, a gateway, and DNS through DHCP, but another
backend may supply different values.

## Updating lwIP

Resolve the intended signed or annotated upstream tag, record its commit,
and compare it with the current import.  From a temporary checkout outside
the kernel tree:

```bash
git clone https://github.com/lwip-tcpip/lwip.git /tmp/lwip-update
git -C /tmp/lwip-update checkout <tag>
git -C /tmp/lwip-update rev-parse HEAD
```

Replace only `COPYING`, `src/api`, `src/core`, `src/include`, and
`src/netif/ethernet.c`.  Do not overwrite `net/lwip/port/` or the Caffeinix
Makefiles under `upstream/`.  Update `UPSTREAM`, inspect upstream option and
API changes, then run the UAPI, host ring, QEMU networking, offline-NIC, SMP,
and filesystem-consistency tests.

Keep upstream C files unchanged unless an independently explained upstream
compatibility patch is unavoidable.  Caffeinix-specific headers and libc
shims belong under `port/`.

The socket UAPI patch extends lwIP's netconn disconnect operation to RAW
PCBs.  This maps Linux `connect(AF_UNSPEC)` onto lwIP's existing
`raw_disconnect()` operation.  It also keeps TCP receive data queued before
`shutdown(SHUT_RD)` readable instead of invalidating the receive mailbox.
Both changes must be revalidated when the import changes.

## Debugging

Boot prints `lwIP: attached <name>` for every interface and prints the DHCP
IPv4 address when it becomes usable.  A missing NIC or DHCP server must still
reach the shell.  Packet validation and driver drops are counted in the
`net_device` statistics, while lwIP statistics and sanity checks are enabled
by the current debug configuration.
